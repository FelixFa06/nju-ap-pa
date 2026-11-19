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
        {
            const std::string accept = message.get("accept");
            if (accept != "yes" && accept != "no")
                fail(seat, "challenge needs accept=yes|no");
            else
                apply_challenge(seat, accept == "yes");
        }
        else // SWAP
            apply_swap(seat, find_seat(message.get("player")));
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
    m_game.uno_reportable.clear();
    clear_waiting();
    reset_chain();
}

void HostSession::reset_chain() {
    m_game.chain_has_d2 = false;
    m_game.last_penalty.clear();
}

void HostSession::clear_waiting() {
    m_game.waiting_challenge = false;
    m_game.challenge_victim = -1;
    m_game.challenge_offender = -1;
    m_game.challenge_legal = false;
    m_game.challenge_color = 0;
    m_game.waiting_swap = false;
    m_game.swap_player = -1;
}

bool HostSession::card_playable(const Card& card, const Card& top) const {
    if (!card.valid()) return false;
    if (card.wild()) return true;
    // 引牌是 Wild 时还没有生效颜色：由庄家打出第一张牌来决定颜色。
    if (m_game.active_color == 0 && top.valid() && top.wild()) return true;
    // top 是万能牌时按生效颜色判断；否则生效颜色就是 top 的颜色。
    if (m_game.active_color != 0 && card.color == m_game.active_color)
        return true;
    if (top.valid() && !top.wild() && card.face == top.face) return true;
    return false;
}

bool HostSession::has_color_in_hand(int seat, char color) const {
    if (color == 0) return false;
    if (seat < 0 || seat >= static_cast<int>(m_players.size())) return false;
    for (const std::string& name : m_players[seat].hand) {
        const Card card = parse_card(name);
        if (card.valid() && !card.wild() && card.color == color) return true;
    }
    return false;
}

void HostSession::send_all_hands() {
    for (int seat = 0; seat < static_cast<int>(m_players.size()); ++seat)
        send_hand(seat);
}

void HostSession::close_uno_window(int actor) {
    // 目标的下家出牌或抽牌之后，漏喊举报窗口关闭。
    for (int seat = 0; seat < static_cast<int>(m_game.uno_reportable.size());
         ++seat)
        if (m_game.uno_reportable[seat] && next_seat(seat) == actor)
            m_game.uno_reportable[seat] = false;
}

void HostSession::mark_one_card(int seat) {
    if (seat < 0 || seat >= static_cast<int>(m_players.size())) return;
    const std::size_t size = m_players[seat].hand.size();
    if (size == 1) {
        m_game.uno_declared[seat] = false;
        m_game.uno_reportable[seat] = true;
    } else {
        m_game.uno_declared[seat] = false;
        m_game.uno_reportable[seat] = false;
    }
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
    m_game.uno_reportable.assign(count, false);

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
    if (m_game.waiting_challenge) {
        fail(seat, "answer the +4 challenge first");
        return false;
    }
    if (m_game.waiting_swap) {
        fail(seat, "the player of 7 must choose a swap target first");
        return false;
    }
    if (seat != m_game.current) {
        fail(seat, "it is not your turn");
        return false;
    }

    const std::vector<std::string> names = split_card_list(cards_text);
    if (names.empty()) {
        fail(seat, "PLAY needs at least one card");
        return false;
    }

    std::vector<Card> cards;
    std::vector<std::string> texts;
    for (const std::string& name : names) {
        const Card card = parse_card(name);
        if (!card.valid()) {
            fail(seat, "unknown card: " + name);
            return false;
        }
        cards.push_back(card);
        texts.push_back(card.text());
    }

    // 抽牌之后本轮只能出抽到的那一张。
    if (m_game.drew_this_turn &&
        (cards.size() != 1 || texts.front() != m_game.drawn_card)) {
        fail(seat, "after drawing you may only play the drawn card");
        return false;
    }

    auto& hand = m_players[seat].hand;
    {
        std::vector<std::string> pool = hand;
        for (const std::string& text : texts) {
            const auto it = std::find(pool.begin(), pool.end(), text);
            if (it == pool.end()) {
                fail(seat, "you do not have " + text);
                return false;
            }
            pool.erase(it);
        }
    }

    const bool stacking = m_game.pending > 0;
    const Card top_before = parse_card(m_game.top);
    const char color_before = m_game.active_color;
    const std::string face = cards.front().face;
    char chosen_color = 0;

    if (cards.size() == 1) {
        const Card& card = cards.front();
        if (stacking) {
            // 叠加只看牌型，不看颜色。
            if (card.face != "D2" && card.face != "W4") {
                fail(seat, "you must draw or stack a penalty card");
                return false;
            }
            if (m_game.last_penalty == "W4") {
                if (card.face != "W4") {
                    fail(seat, "only a W4 can stack on a W4");
                    return false;
                }
                if (!m_game.chain_has_d2) {
                    fail(seat, "this W4 chain cannot be stacked");
                    return false;
                }
            }
        } else if (!card_playable(card, top_before)) {
            fail(seat, "that card does not match the table");
            return false;
        }

        if (card.wild()) {
            chosen_color = parse_color(color_text);
            if (chosen_color == 0) {
                fail(seat, "wild card needs a color");
                return false;
            }
        }
    } else {
        // 多张出牌只允许在普通回合使用。
        if (stacking) {
            fail(seat, "you can only stack one penalty card at a time");
            return false;
        }

        bool same_face = true;
        bool same_color = true;
        bool all_number = true;
        bool distinct_colors = true;
        for (std::size_t i = 0; i < cards.size(); ++i) {
            if (cards[i].face != cards[0].face) same_face = false;
            if (cards[i].color != cards[0].color) same_color = false;
            for (std::size_t j = 0; j < i; ++j)
                if (cards[j].color == cards[i].color) distinct_colors = false;
            const std::string& f = cards[i].face;
            if (f.size() != 1 ||
                !std::isdigit(static_cast<unsigned char>(f[0])))
                all_number = false;
        }

        if (same_color && same_face && (all_number || face == "SK")) {
            // 同色同数字（可多张）或同色 SK 叠出。
            if (!card_playable(cards.front(), top_before)) {
                fail(seat, "that combination does not match the table");
                return false;
            }
        } else if (same_face && all_number && cards.size() >= 3 &&
                   distinct_colors) {
            // 同数字不同色，至少三张，必须指定颜色。
            bool matches = false;
            for (const Card& card : cards)
                if (card_playable(card, top_before)) matches = true;
            if (!matches) {
                fail(seat, "that combination does not match the table");
                return false;
            }
            chosen_color = parse_color(color_text);
            if (chosen_color == 0) {
                fail(seat, "that combination needs a color");
                return false;
            }
        } else {
            fail(seat, "that is not a legal combination of cards");
            return false;
        }

        // 最后一张牌只能单出：多张出牌之后手里至少还要留一张。
        if (hand.size() == cards.size()) {
            fail(seat, "the last card must be played alone");
            return false;
        }
    }

    for (const std::string& text : texts) {
        const auto it = std::find(hand.begin(), hand.end(), text);
        if (it != hand.end()) hand.erase(it);
    }

    const char new_color = chosen_color != 0 ? chosen_color : cards[0].color;
    m_game.top = cards.front().text();
    m_game.active_color = new_color;
    // 多张出牌时把每张都放进弃牌堆；最后压入的必须与 top 一致。
    for (std::size_t i = 1; i < cards.size(); ++i)
        m_game.discard.push_back(cards[i].text());
    m_game.discard.push_back(cards.front().text());

    Message played("PLAYED");
    played.set("player", seat).set("cards", join_cards(texts));
    if (chosen_color != 0) played.set("color", color_name(chosen_color));
    announce(played);

    m_game.drew_this_turn = false;
    m_game.drawn_card.clear();
    close_uno_window(seat);

    if (hand.empty()) {
        finish_round(seat);
        return true;
    }
    mark_one_card(seat);

    // ---- 罚抽链 ---------------------------------------------------------
    if (face == "W4") {
        if (stacking) {
            // 叠加出来的 W4 不可质疑，也不打开质疑窗口。
            m_game.pending += 4;
            m_game.last_penalty = "W4";
            m_game.current = next_seat(seat);
            broadcast_turn();
            return true;
        }

        // 普通 W4：先询问受害者是否质疑，然后暂停（不广播 TURN）。
        // 合法性只看出牌前的手牌：有没有与参照牌同色的非万能牌。
        const bool w4_legal = !has_color_in_hand(seat, color_before);
        const int victim = next_seat(seat);
        m_game.pending = 4;
        reset_chain();
        m_game.last_penalty = "W4";
        m_game.current = victim;
        m_game.waiting_challenge = true;
        m_game.challenge_victim = victim;
        m_game.challenge_offender = seat;
        m_game.challenge_legal = w4_legal;
        m_game.challenge_color = chosen_color;

        announce(Message("CHALLENGE")
                     .set("player", victim)
                     .set("ask", "w4")
                     .set("cards", 4)
                     .set("color", color_name(chosen_color)));
        return true;
    }

    if (face == "D2") {
        if (stacking) {
            m_game.pending += 2;
            m_game.chain_has_d2 = true;
            m_game.last_penalty = "D2";
        } else {
            reset_chain();
            m_game.pending = 2;
            m_game.chain_has_d2 = true;
            m_game.last_penalty = "D2";
        }
        m_game.current = next_seat(seat);
        broadcast_turn();
        return true;
    }

    // ---- 普通出牌的回合推进 --------------------------------------------
    m_game.pending = 0;
    reset_chain();

    if (face == "SK") {
        // 出 n 张 SK 跳过 n 个玩家。
        m_game.current = next_seat(seat, static_cast<int>(cards.size()) + 1);
        broadcast_turn();
    } else if (face == "RV") {
        if (static_cast<int>(m_players.size()) == 2) {
            m_game.current = next_seat(seat, 2); // 两人局 Reverse 等同 Skip
            broadcast_turn();
        } else {
            m_game.direction = -m_game.direction;
            m_game.current = next_seat(seat);
            broadcast_turn();
        }
    } else if (face == "0") {
        apply_zero_swap();
        m_game.current = next_seat(seat);
        broadcast_turn();
    } else if (face == "7") {
        open_seven_window(seat);
    } else {
        m_game.current = next_seat(seat);
        broadcast_turn();
    }
    return true;
}

void HostSession::apply_zero_swap() {
    const int count = static_cast<int>(m_players.size());
    std::vector<std::vector<std::string>> old;
    old.reserve(m_players.size());
    for (const Player& player : m_players) old.push_back(player.hand);

    // 每位玩家的手牌交给当前方向上的下一位。
    for (int seat = 0; seat < count; ++seat)
        m_players[next_seat(seat)].hand = old[seat];

    announce(Message("SWAP").set("all", "yes"));
    send_all_hands();
    for (int seat = 0; seat < count; ++seat) mark_one_card(seat);
}

void HostSession::open_seven_window(int seat) {
    m_game.waiting_swap = true;
    m_game.swap_player = seat;
    announce(Message("SWAP").set("player", seat).set("waiting", "yes"));
}

void HostSession::apply_draw(int seat) {
    if (m_game.waiting_challenge) {
        fail(seat, "answer the +4 challenge first");
        return;
    }
    if (m_game.waiting_swap) {
        fail(seat, "the player of 7 must choose a swap target first");
        return;
    }
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
        reset_chain();
        close_uno_window(seat);
        const std::size_t before = m_players[seat].hand.size();
        draw_into_hand(seat, requested);
        const int drawn =
            static_cast<int>(m_players[seat].hand.size() - before);
        mark_one_card(seat);
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
    mark_one_card(seat);
    m_game.drew_this_turn = true;
    m_game.drawn_card = card;
    close_uno_window(seat);

    Message message("DRAWN");
    message.set("player", seat).set("count", 1);
    announce(message);
    send_hand(seat);
    broadcast_turn();
}

void HostSession::apply_pass(int seat) {
    if (m_game.waiting_challenge) {
        fail(seat, "answer the +4 challenge first");
        return;
    }
    if (m_game.waiting_swap) {
        fail(seat, "the player of 7 must choose a swap target first");
        return;
    }
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
    if (m_game.waiting_challenge || m_game.waiting_swap ||
        m_game.pending > 0) {
        fail(seat, "wait until the current decision is resolved");
        return;
    }
    if (target < 0 || target >= static_cast<int>(m_players.size())) {
        fail(seat, "no such player");
        return;
    }

    auto& hand = m_players[target].hand;
    if (target == seat) {
        if (hand.size() == 1) {
            m_game.uno_declared[target] = true;
            m_game.uno_reportable[target] = false;
            announce(Message("UNO").set("player", target));
        } else {
            m_game.uno_declared[target] = false;
            announce(Message("UNOPENALTY").set("player", target).set("count", 2));
            draw_into_hand(target, 2);
            mark_one_card(target);
            send_hand(target);
        }
        return;
    }

    // 举报别人漏喊：目标必须刚出到 1 张、还没喊过，且窗口未关闭。
    if (hand.size() == 1 && m_game.uno_reportable[target] &&
        !m_game.uno_declared[target]) {
        announce(Message("UNOPENALTY").set("player", target).set("count", 2));
        draw_into_hand(target, 2);
        m_game.uno_declared[target] = false;
        m_game.uno_reportable[target] = false;
        send_hand(target);
    } else {
        fail(seat, "the UNO report is not valid");
    }
}

void HostSession::apply_challenge(int seat, bool accept) {
    // ---- 普通 W4：正在等受害者的回答 ------------------------------------
    if (m_game.waiting_challenge) {
        if (seat != m_game.challenge_victim) {
            fail(seat, "only the penalized player can answer the challenge");
            return;
        }

        const int victim = m_game.challenge_victim;
        const int offender = m_game.challenge_offender;
        const bool legal = m_game.challenge_legal;
        const char w4_color = m_game.challenge_color;
        clear_waiting();
        m_game.pending = 0;

        const char* result = nullptr;
        int drawer = victim;
        int draw_count = 0;
        if (!accept) {
            result = "draw4";
            draw_count = 4;
            m_game.current = next_seat(victim);
        } else if (legal) {
            result = "draw6";
            draw_count = 6;
            m_game.current = next_seat(victim);
        } else {
            result = "illegal";
            draw_count = 4;
            drawer = offender;
            m_game.current = victim;
            // 无论结果如何，W4 指定的颜色继续生效。
            m_game.active_color = w4_color;
        }

        draw_into_hand(drawer, draw_count);
        mark_one_card(drawer);
        reset_chain();

        announce(Message("CHALLENGE")
                     .set("player", victim)
                     .set("accept", accept ? "yes" : "no")
                     .set("result", result));
        send_hand(drawer);
        broadcast_turn();
        return;
    }

    // 叠加出来的 W4 不可质疑：罚抽链进行中没有质疑窗口。
    fail(seat, "there is no +4 challenge window");
}

void HostSession::apply_swap(int seat, int target) {
    if (!m_game.waiting_swap) {
        fail(seat, "there is no swap window open");
        return;
    }
    if (seat != m_game.swap_player) {
        fail(seat, "only the player of 7 can choose a swap target");
        return;
    }
    if (target < 0 || target >= static_cast<int>(m_players.size())) {
        fail(seat, "no such player");
        return;
    }
    if (target == seat) {
        fail(seat, "choose another player to swap hands with");
        return;
    }

    const int player = m_game.swap_player;
    std::swap(m_players[player].hand, m_players[target].hand);
    m_game.waiting_swap = false;
    m_game.swap_player = -1;

    send_hand(player);
    send_hand(target);
    mark_one_card(player);
    mark_one_card(target);

    announce(Message("SWAP").set("player", player).set("with", target));
    m_game.current = next_seat(player);
    broadcast_turn();
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
    apply_challenge(0, args[1] == "yes");
    return Result::Continue;
}

Session::Result HostSession::swap(const Args& args) {
    apply_swap(0, find_seat(args[1]));
    return Result::Continue;
}

} // 命名空间 uno
