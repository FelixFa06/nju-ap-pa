#include "host_session.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <utility>

namespace uno {

HostSession::HostSession(std::string name, std::istream& in, std::ostream& out)
    : Session(Role::Host, std::move(name), in, out) {
    add_lobby_command("start", "start",
                      "start the game and deal the cards (host only)",
                      [this](const Args& args) { return start(args); });
    add_lobby_command("kick", "kick <player_id|player_name>",
                      "remove a player from the room (host only)",
                      [this](const Args& args) { return kick(args); }, 1, 1);
    initialize_players();
}

HostSession::~HostSession() = default;

// ---------------------------------------------------------------------------
// 传输初始化。这段是框架的一部分：房间路径推导、残留 socket 清理、权限、
// 非阻塞设置都已经处理好，你不需要改动。
// ---------------------------------------------------------------------------
bool HostSession::listen_room(const std::string& room, std::string& err) {
    const std::string path = room_socket_path(room, err);
    if (path.empty()) return false;

    auto server = std::make_unique<UnixServer>();
    if (!server->listen_path(path, err)) return false;

    m_room_path = path;
    m_server = std::move(server);
    loop().set_server(m_server.get());
    return true;
}

bool HostSession::listen_tcp(int port, std::string& err) {
    auto server = std::make_unique<TcpServer>();
    if (!server->listen(port, err)) return false;

    m_server = std::move(server);
    loop().set_server(m_server.get());
    return true;
}

// ---------------------------------------------------------------------------
// 事件钩子：这些是你要实现的部分。
// ---------------------------------------------------------------------------
void HostSession::on_peer_join(PeerId who) {
    // info("TODO(host): a client connected as peer " + who.str() +
    //      " -- greet it, ask for a name, and update the player list");
    (void)who;
}

void HostSession::on_peer_message(PeerId from, const Message& message) {
    if (message.type == "JOIN")
    {
        const std::string name = message.get("name");
        if (phase() == Phase::Game)
            send_to(from, Message("ERROR").set("msg", "the game has already started"));
        else if (name.empty())
            send_to(from, Message("ERROR").set("msg", "JOIN needs a name"));
        else if (m_seat_of.count(from) > 0)
            send_to(from, Message("ERROR").set("msg", "you have already joined"));
        else if (name_taken(name, from))
            send_to(from, Message("ERROR").set("msg", "name is taken: " + name));
        else
        {
            const int seat_id = append_player(from, name);
            send_to(from, Message("WELCOME").set("id",seat_id).set("name",name));
            announce(show_players());
        }
    }
    else if (message.type == "RENAME")
    {
        const std::string name = message.get("name");
        if (phase() == Phase::Game)
            send_to(from, Message("ERROR").set("msg", "cannot rename during a game"));
        else if (name.empty())
            send_to(from, Message("ERROR").set("msg", "RENAME needs a name"));
        else if (m_seat_of.count(from) == 0)
            send_to(from, Message("ERROR").set("msg", "RENAME before JOIN"));
        else if (name_taken(name, from))
            send_to(from, Message("ERROR").set("msg", "name is taken: " + name));
        else
        {
            rename_player(from, name);
            announce(show_players());
        }
    }
    else if (message.type == "PLAY" || message.type == "DRAW" ||
             message.type == "PASS" || message.type == "UNO" ||
             message.type == "CHALLENGE" || message.type == "SWAP")
    {
        const int seat = seat_of(from);
        if (seat < 0)
        {
            send_to(from, Message("ERROR").set("msg", "you have not joined the room"));
            return;
        }
        if (phase() != Phase::Game)
        {
            send_to(from, Message("ERROR").set("msg", "the game has not started"));
            return;
        }

        if (message.type == "PLAY")
            apply_play(seat, message.get("cards"), message.get("color"));
        else if (message.type == "DRAW")
            apply_draw(seat);
        else if (message.type == "PASS")
            apply_pass(seat);
        else if (message.type == "UNO")
        {
            const int target = message.has("player")
                                   ? find_seat(message.get("player"))
                                   : seat;
            apply_uno(seat, target);
        }
        else if (message.type == "CHALLENGE")
            fail(seat, "challenge is only used by the extended rules");
        else
            fail(seat, "swap is only used by the extended rules");
    }
    else
        send_to(from, Message("ERROR").set("msg", "unknown message type: " + message.type));
}

void HostSession::on_peer_leave(PeerId who) {
    const int seat = seat_of(who);
    if (seat < 0) return;

    // 一局进行中有人掉线，整局无法继续：结束本局并回到大厅。
    if (phase() == Phase::Game)
    {
        announce(Message("ABORTED"));
        reset_game();
        return_to_lobby();
    }

    drop_player(who);
    announce(show_players());
}

// ---------------------------------------------------------------------------
// Player List 相关操作
// ---------------------------------------------------------------------------
void HostSession::initialize_players() {
    m_players.clear();
    m_players.push_back(Player(PeerId(0),session_name()));
    m_seat_of[PeerId(0)] = 0;
}

Message HostSession::show_players() const {
    Message mes("PLAYERS");
    mes.set("count", (int)m_players.size());
    for (int i = 0; i < (int)m_players.size();i++)
        mes.set(std::to_string(i), m_players[i].name);
    return Message(mes);
}

int HostSession::append_player(PeerId who, std::string name) {
    Player player(who, name);
    int seat_id = (int)m_players.size();
    m_players.push_back(player);
    m_seat_of[who] = seat_id;
    return seat_id; // 返回座位号
}

void HostSession::rename_player(PeerId who, std::string name) {
    if (m_seat_of.find(who)==m_seat_of.end())
        return;
    int seat_id = m_seat_of[who];
    m_players[seat_id].name = name;
}

void HostSession::drop_player(PeerId who) {
    if (m_seat_of.find(who)==m_seat_of.end())
        return;
    int seat_id = m_seat_of[who];
    m_seat_of.erase(who);
    m_players.erase(m_players.begin() + seat_id);
    reindex_players();
}

void HostSession::reindex_players() {
    m_seat_of.clear();
    for (int i = 0; i < (int)m_players.size(); i++)
        m_seat_of[m_players[i].who] = i;
}

int HostSession::find_seat(const std::string& key) const { // key 可以为座位号或玩家名字
    bool numeric = !key.empty();
    for (char c : key) if (!std::isdigit((unsigned char)c)) { numeric = false; break; }
    if (numeric)
    {
        int seat_id = 0;
        for (char c : key)
        {
            seat_id = seat_id * 10 + (c - '0');
            if (seat_id >= (int)m_players.size())
                return -1;
        }
        return seat_id;
    }
    for (int i = 0; i < (int)m_players.size(); i++)
        if (m_players[i].name == key)
            return i;
    return -1;
}

bool HostSession::name_taken(const std::string& name, PeerId except) const {
    for (const Player& player : m_players)
        if (player.who != except && player.name == name) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 牌桌状态：座位换算、发牌与消息投递
// ---------------------------------------------------------------------------
namespace {

// 标准 108 张牌组，顺序见 docs/manual.pdf「牌堆与确定性洗牌」。
std::vector<std::string> build_full_deck() {
    std::vector<std::string> deck;
    for (char color : {'R', 'Y', 'G', 'B'}) {
        deck.push_back(std::string(1, color) + "0");
        for (int n = 1; n <= 9; ++n) {
            const std::string card = std::string(1, color) + std::to_string(n);
            deck.push_back(card);
            deck.push_back(card);
        }
        for (const char* face : {"SK", "RV", "D2"}) {
            const std::string card = std::string(1, color) + face;
            deck.push_back(card);
            deck.push_back(card);
        }
    }
    for (int i = 0; i < 4; ++i) deck.push_back("W");
    for (int i = 0; i < 4; ++i) deck.push_back("W4");
    return deck;
}

} // 匿名命名空间

int HostSession::seat_of(PeerId who) const {
    const auto it = m_seat_of.find(who);
    return it == m_seat_of.end() ? -1 : it->second;
}

int HostSession::next_seat(int from, int steps) const {
    const int count = static_cast<int>(m_players.size());
    if (count <= 0) return 0;
    int seat = (from + m_game.direction * steps) % count;
    if (seat < 0) seat += count;
    return seat;
}

void HostSession::fail(int seat, const std::string& message) {
    if (seat < 0 || seat >= static_cast<int>(m_players.size()) ||
        !m_players[seat].who.valid())
        error(message);
    else
        send_to(m_players[seat].who, Message("ERROR").set("msg", message));
}

std::string HostSession::hand_cards(int seat) const {
    if (seat < 0 || seat >= static_cast<int>(m_players.size())) return {};
    return join_cards(m_players[seat].hand);
}

void HostSession::send_hand(int seat) {
    if (seat < 0 || seat >= static_cast<int>(m_players.size())) return;
    const std::string cards = hand_cards(seat);
    if (!m_players[seat].who.valid()) {
        out() << "HAND cards=" << cards << "\n";
        out().flush();
    } else {
        send_to(m_players[seat].who, Message("HAND").set("cards", cards));
    }
}

void HostSession::announce(Message message) {
    // host 自己不在 EventLoop 的 peers 里，broadcast() 不会回显给自己；
    // 这里先打印一份，再投递给所有客户端。
    out() << message.serialize() << "\n";
    out().flush();
    broadcast(std::move(message));
}

void HostSession::broadcast_turn() {
    Message turn("TURN");
    turn.set("player", m_game.current);
    turn.set("top", m_game.top);
    turn.set("direction", m_game.direction);
    if (m_game.pending > 0) turn.set("pending", m_game.pending);
    announce(std::move(turn));
}

void HostSession::reset_game() {
    for (Player& player : m_players) player.hand.clear();
    m_game.draw_pile.clear();
    m_game.discard.clear();
    m_game.top.clear();
    m_game.active_color = 0;
    m_game.current = 0;
    m_game.direction = 1;
    m_game.pending = 0;
    m_game.drew_this_turn = false;
    m_game.drawn_card.clear();
    m_game.uno_declared.clear();
}

std::string HostSession::draw_one() {
    if (m_game.draw_pile.empty()) reshuffle_discard();
    if (m_game.draw_pile.empty()) return {};

    std::string card = std::move(m_game.draw_pile.front());
    m_game.draw_pile.erase(m_game.draw_pile.begin());
    return card;
}

void HostSession::reshuffle_discard() {
    if (m_game.discard.size() <= 1) return;

    // 保留最上面那张作参照牌，其余弃牌重洗成摸牌堆。
    const std::string top = m_game.discard.back();
    std::vector<std::string> rest(m_game.discard.begin(),
                                  m_game.discard.end() - 1);
    m_game.discard.assign(1, top);
    std::shuffle(rest.begin(), rest.end(), m_game.rng);
    m_game.draw_pile = std::move(rest);
}

void HostSession::draw_into_hand(int seat, int count) {
    if (seat < 0 || seat >= static_cast<int>(m_players.size())) return;
    for (int i = 0; i < count; ++i) {
        const std::string card = draw_one();
        if (card.empty()) break;
        m_players[seat].hand.push_back(card);
    }
}

void HostSession::apply_starter_effect() {
    m_game.current = 0;
    m_game.direction = 1;
    m_game.pending = 0;
    m_game.active_color = 0;
    m_game.drew_this_turn = false;
    m_game.drawn_card.clear();

    const Card top = parse_card(m_game.top);
    if (!top.valid()) return;

    if (!top.wild()) m_game.active_color = top.color;

    if (top.face == "SK") {
        m_game.current = next_seat(0); // 跳过庄家
    } else if (top.face == "RV") {
        if (static_cast<int>(m_players.size()) == 2)
            m_game.current = next_seat(0); // 两人局 Reverse 等同 Skip
        else
            m_game.direction = -1;
    } else if (top.face == "D2" || top.face == "W4") {
        const int count = top.face == "D2" ? 2 : 4;
        draw_into_hand(0, count); // 庄家罚抽并跳过
        m_game.current = next_seat(0);
    }
    // 引牌是 W 时没有生效颜色，由庄家打出的第一张牌决定。
}

void HostSession::start_round(std::vector<std::string> deck) {
    const int count = static_cast<int>(m_players.size());
    reset_game();
    m_game.uno_declared.assign(count, false);

    if (has_seed())
        m_game.rng.seed(seed());
    else
        m_game.rng.seed(std::random_device{}());

    // 给了 --deck 就严格按给定顺序发牌；否则按 seed 确定性洗牌。
    if (!has_deck_override())
        std::shuffle(deck.begin(), deck.end(), m_game.rng);

    for (int seat = 0; seat < count; ++seat) {
        for (int i = 0; i < 7; ++i)
            m_players[seat].hand.push_back(deck[seat * 7 + i]);
    }

    m_game.top = deck[7 * count];
    m_game.discard.push_back(m_game.top);
    m_game.draw_pile.assign(deck.begin() + 7 * count + 1, deck.end());

    apply_starter_effect();

    for (int seat = 0; seat < count; ++seat) send_hand(seat);
    announce(Message("START"));
    broadcast_turn();
    enter_game();
}

// ---------------------------------------------------------------------------
// 本地命令行：host 玩家自己敲的命令。
// ---------------------------------------------------------------------------
Session::Result HostSession::start(const Args&) {
    if (static_cast<int>(m_players.size()) < 2) {
        error("players not enough (less than 2)");
        return Result::Continue;
    }

    std::vector<std::string> deck =
        has_deck_override() ? deck_override() : build_full_deck();
    const std::size_t needed = static_cast<std::size_t>(m_players.size()) * 7 + 1;
    if (deck.size() < needed) {
        error("the deck has " + std::to_string(deck.size()) +
              " card(s), but at least " + std::to_string(needed) +
              " are needed");
        return Result::Continue;
    }

    start_round(std::move(deck));
    return Result::StartGame;
}

Session::Result HostSession::kick(const Args& args) {
    // info("TODO(host): remove player " + args[1] + " and broadcast PLAYERS");
    int seat_id = find_seat(args[1]);
    if (seat_id < 0)
        error("no such player: " + args[1]); 
    else if (!m_players[seat_id].who.valid())
        error("cannot kick that player");
    else
        loop().close_peer(m_players[seat_id].who);
    return Result::Continue;
}

Session::Result HostSession::rename(const Args& args) {
    // info("TODO(host): rename player to " + args[1] + " and broadcast PLAYERS");
    if (name_taken(args[1], PeerId(0)))
    {
        error("name is taken: " + args[1]);
        return Result::Continue;
    }
    rename_player(PeerId(0), args[1]);
    announce(show_players());
    return Result::Continue;
}

// ---------------------------------------------------------------------------
// 规则引擎
// ---------------------------------------------------------------------------
bool HostSession::apply_play(int seat, const std::string& cards_text,
                             const std::string& color_text) {
    if (seat != m_game.current) {
        fail(seat, "it is not your turn");
        return false;
    }

    const std::vector<std::string> names = split_card_list(cards_text);
    if (names.size() != 1) {
        fail(seat, "stage 2 allows exactly one card per play");
        return false;
    }

    const Card card = parse_card(names.front());
    if (!card.valid()) {
        fail(seat, "unknown card: " + names.front());
        return false;
    }

    if (m_game.drew_this_turn && card.text() != m_game.drawn_card) {
        fail(seat, "after drawing you may only play the drawn card");
        return false;
    }

    auto& hand = m_players[seat].hand;
    const auto in_hand = std::find(hand.begin(), hand.end(), card.text());
    if (in_hand == hand.end()) {
        fail(seat, "you do not have " + card.text());
        return false;
    }

    const bool stacking = m_game.pending > 0;
    if (stacking) {
        // 罚抽链上只能继续叠加 D2 / W4，颜色不参与判断。
        if (card.face != "D2" && card.face != "W4") {
            fail(seat, "you must draw or stack a penalty card");
            return false;
        }
    } else {
        const Card top = parse_card(m_game.top);
        bool playable = card.wild();
        if (!playable && top.valid()) {
            if (top.wild())
                playable = m_game.active_color == 0 ||
                           card.color == m_game.active_color;
            else
                playable = card.color == top.color || card.face == top.face;
        }
        if (!playable) {
            fail(seat, "that card does not match the table");
            return false;
        }
    }

    char chosen_color = 0;
    if (card.wild()) {
        chosen_color = parse_color(color_text);
        if (chosen_color == 0) {
            fail(seat, "wild card needs a color");
            return false;
        }
    }

    hand.erase(in_hand);
    m_game.top = card.text();
    m_game.active_color = card.wild() ? chosen_color : card.color;
    m_game.discard.push_back(m_game.top);

    Message played("PLAYED");
    played.set("player", seat).set("cards", card.text());
    if (card.wild()) played.set("color", color_name(chosen_color));
    announce(played);

    m_game.drew_this_turn = false;
    m_game.drawn_card.clear();

    if (hand.empty()) {
        finish_round(seat);
        return true;
    }

    if (stacking) {
        m_game.pending += card.face == "W4" ? 4 : 2;
        m_game.current = next_seat(seat);
        broadcast_turn();
        return true;
    }

    m_game.pending = 0;
    if (card.face == "SK") {
        m_game.current = next_seat(seat, 2);
    } else if (card.face == "RV") {
        if (static_cast<int>(m_players.size()) == 2)
            m_game.current = next_seat(seat, 2); // 两人局 Reverse 等同 Skip
        else {
            m_game.direction = -m_game.direction;
            m_game.current = next_seat(seat);
        }
    } else if (card.face == "D2") {
        m_game.pending = 2;
        m_game.current = next_seat(seat);
    } else if (card.face == "W4") {
        m_game.pending = 4;
        m_game.current = next_seat(seat);
    } else {
        m_game.current = next_seat(seat);
    }
    broadcast_turn();
    return true;
}

void HostSession::apply_draw(int seat) {
    if (seat != m_game.current) {
        fail(seat, "it is not your turn");
        return;
    }
    if (m_game.drew_this_turn) {
        fail(seat, "you have already drawn this turn");
        return;
    }

    if (m_game.pending > 0) {
        // 吃下累计罚抽，并被跳过。
        const int requested = m_game.pending;
        m_game.pending = 0;
        const std::size_t before = m_players[seat].hand.size();
        draw_into_hand(seat, requested);
        const int drawn =
            static_cast<int>(m_players[seat].hand.size() - before);
        Message message("DRAWN");
        message.set("player", seat).set("count", drawn);
        announce(message);
        send_hand(seat);
        m_game.current = next_seat(seat);
        broadcast_turn();
        return;
    }

    const std::string card = draw_one();
    if (card.empty()) {
        fail(seat, "there are no cards left to draw");
        return;
    }

    m_players[seat].hand.push_back(card);
    m_game.drew_this_turn = true;
    m_game.drawn_card = card;

    Message message("DRAWN");
    message.set("player", seat).set("count", 1);
    announce(message);
    send_hand(seat);
}

void HostSession::apply_pass(int seat) {
    if (seat != m_game.current) {
        fail(seat, "it is not your turn");
        return;
    }
    if (m_game.pending > 0) {
        fail(seat, "draw the penalty before passing");
        return;
    }
    if (!m_game.drew_this_turn) {
        fail(seat, "pass is only allowed after drawing");
        return;
    }

    m_game.drew_this_turn = false;
    m_game.drawn_card.clear();
    m_game.current = next_seat(seat);
    broadcast_turn();
}

void HostSession::apply_uno(int seat, int target) {
    if (target < 0 || target >= static_cast<int>(m_players.size())) {
        fail(seat, "no such player");
        return;
    }

    auto& hand = m_players[target].hand;
    if (target == seat) {
        if (hand.size() == 1) {
            m_game.uno_declared[target] = true;
            announce(Message("UNO").set("player", target));
        } else {
            m_game.uno_declared[target] = false;
            announce(Message("UNOPENALTY").set("player", target).set("count", 2));
            draw_into_hand(target, 2);
            send_hand(target);
        }
        return;
    }

    // 举报别人漏喊：目标必须正好 1 张且还没喊过。
    if (hand.size() == 1 && !m_game.uno_declared[target]) {
        announce(Message("UNOPENALTY").set("player", target).set("count", 2));
        draw_into_hand(target, 2);
        send_hand(target);
    } else {
        fail(seat, "the UNO report is not valid");
    }
}

void HostSession::finish_round(int winner) {
    int points = 0;
    for (int seat = 0; seat < static_cast<int>(m_players.size()); ++seat)
        if (seat != winner) points += hand_value(m_players[seat].hand);

    announce(Message("GAMEOVER").set("winner", winner).set("points", points));
    reset_game();
    return_to_lobby();
}

Session::Result HostSession::play(const Args& args) {
    apply_play(0, args[1], args.size() > 2 ? args[2] : std::string());
    return Result::Continue;
}

Session::Result HostSession::cards(const Args&) {
    send_hand(0);
    return Result::Continue;
}

Session::Result HostSession::draw(const Args&) {
    apply_draw(0);
    return Result::Continue;
}

Session::Result HostSession::pass(const Args&) {
    apply_pass(0);
    return Result::Continue;
}

Session::Result HostSession::uno(const Args& args) {
    const int target = args.size() > 1 ? find_seat(args[1]) : 0;
    apply_uno(0, target);
    return Result::Continue;
}

Session::Result HostSession::challenge(const Args& args) {
    if (!valid_challenge_answer(args)) return Result::Continue;
    fail(0, "challenge is only used by the extended rules");
    return Result::Continue;
}

Session::Result HostSession::swap(const Args&) {
    fail(0, "swap is only used by the extended rules");
    return Result::Continue;
}

} // 命名空间 uno
