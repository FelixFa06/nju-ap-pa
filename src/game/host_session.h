#pragma once

#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "game/cards.h"
#include "game/session.h"
#include "protocol/net.h"
#include "protocol/unix.h"

namespace uno {

// ---------------------------------------------------------------------------
// HostSession -- 持有权威状态的一端的会话。
//
// 框架已经替你完成：本地/网络监听、连接的 accept 与编号、断线检测、消息
// 分帧、出站排队。你在这里要做的只有「策略」：
//
//   * on_peer_join / on_peer_message / on_peer_leave：一个连接来、说话、走；
//   * start / kick 以及共用命令的 host 侧实现：改状态、决定发给谁；
//   * 玩家与座位模型、规则引擎、回合状态机。
//
// 与游戏规则相关的工作先用 info("TODO(host): ...") 标记，你在对应函数里
// 替换成真正的实现。
// ---------------------------------------------------------------------------
class HostSession : public Session {
public:
    explicit HostSession(std::string name, std::istream& in = std::cin,
                         std::ostream& out = std::cout);
    ~HostSession() override;

    // 本地房间：在 <运行时目录>/uno/<room>.sock 上监听，客户端用 --join=<room>
    // 加入。这是本作业默认的多人方式，同一台机器上的多个进程即可开局。
    bool listen_room(const std::string& room, std::string& err);

    // 网络彩蛋：TCP 监听，客户端用 --host=<ip> 加入。
    bool listen_tcp(int port, std::string& err);

    const std::string& room_path() const { return m_room_path; }

protected:
    // ---- 事件钩子（你要实现）------------------------------------------
    void on_peer_join(PeerId who) override;
    void on_peer_message(PeerId from, const Message& message) override;
    void on_peer_leave(PeerId who) override;

    // ---- Player List 相关操作 ----------------------------------------
    void initialize_players();
    Message show_players() const;
    int append_player(PeerId who, std::string name);
    void rename_player(PeerId who, std::string name);
    void drop_player(PeerId who);
    void reindex_players();
    int find_seat(const std::string &key) const;
    // 名字是否已被别人占用（except 是自己，允许「改成自己的名字」）。
    bool name_taken(const std::string& name, PeerId except) const;

    // ---- host 独有的大厅命令 -------------------------------------------
    Result start(const Args& args);
    Result kick(const Args& args);

    // ---- 与 client 共有的命令，host 侧实现 -----------------------------
    Result rename(const Args& args) override;
    Result play(const Args& args) override;
    Result cards(const Args& args) override;
    Result draw(const Args& args) override;
    Result pass(const Args& args) override;
    Result uno(const Args& args) override;
    Result challenge(const Args& args) override;
    Result swap(const Args& args) override;

private:
    std::unique_ptr<Server> m_server;
    std::string m_room_path;
    struct Player
    {
        PeerId who;
        std::string name;
        std::vector<std::string> hand;
        Player(PeerId _who, std::string _name)
            : who(_who), name(std::move(_name)) {}
    };
    std::vector<Player> m_players;
    std::map<PeerId, int> m_seat_of;

    // ---- 牌桌状态（阶段二 + 阶段三拓展规则）---------------------------
    struct GameState {
        std::vector<std::string> draw_pile; // 摸牌堆（头部是下一张）
        std::vector<std::string> discard;   // 弃牌堆（尾部是当前牌面）
        std::string top;                    // 当前牌面
        char active_color = 0;              // 万能牌生效的颜色
        int current = 0;                    // 当前该谁出牌
        int direction = 1;                  // 1 顺时针，-1 逆时针
        int pending = 0;                    // 累计罚抽张数
        bool drew_this_turn = false;        // 本回合是否已经抽过牌
        std::string drawn_card;             // 本回合抽到的那张牌
        std::vector<bool> uno_declared;     // 每人是否已经喊过 UNO
        std::vector<bool> uno_reportable;   // 刚出到 1 张且未喊，可被举报
        std::mt19937 rng;                   // 洗牌 / 重洗用的确定性随机源

        // ---- 等待窗口 ---------------------------------------------------
        // 刚打出 W4，暂停并等受害者回答质疑。
        bool waiting_challenge = false;
        int challenge_victim = -1;
        int challenge_offender = -1;
        bool challenge_legal = false;   // W4 出牌时手里没有同色牌
        char challenge_color = 0;       // W4 指定的颜色

        // 刚打出 7，等出牌者用 SWAP 指定交换对象。
        bool waiting_swap = false;
        int swap_player = -1;

        // ---- 罚抽链 -----------------------------------------------------
        // 叠加出来的 W4 不可质疑：只有普通（链条首张）W4 会打开质疑窗口。
        bool chain_has_d2 = false;          // 这条链里出现过 D2
        std::string last_penalty;           // 上一张罚抽牌："D2" / "W4"
    };
    GameState m_game;

    int seat_of(PeerId who) const;
    int next_seat(int from, int steps = 1) const;

    void fail(int seat, const std::string& message);
    std::string hand_cards(int seat) const;
    void send_hand(int seat);
    // 广播给所有客户端，同时打印到 host 自己的终端（host 也是玩家）。
    void announce(Message message);
    void broadcast_turn();

    void reset_game();
    void start_round(std::vector<std::string> deck);
    void apply_starter_effect();
    std::string draw_one();
    void reshuffle_discard();
    void draw_into_hand(int seat, int count);

    bool apply_play(int seat, const std::string& cards_text,
                    const std::string& color_text);
    void apply_draw(int seat);
    void apply_pass(int seat);
    void apply_uno(int seat, int target);
    void apply_challenge(int seat, bool accept);
    void apply_swap(int seat, int target);
    void finish_round(int winner);

    // ---- 阶段三辅助 ----------------------------------------------------
    bool card_playable(const Card& card, const Card& top) const;
    bool has_color_in_hand(int seat, char color) const;
    void send_all_hands();
    void close_uno_window(int actor);
    void mark_one_card(int seat);
    void reset_chain();
    void clear_waiting();
    void apply_zero_swap();
    void open_seven_window(int seat);
};

} // 命名空间 uno
