#pragma once

// ---------------------------------------------------------------------------
// 牌名与牌堆的小工具。牌名是协议里的固定表示：
//   R/Y/G/B + 0-9 / SK / RV / D2，以及 W / W4。
// host 用它们做规则判断，client 只用 split/join 维护手牌视图。
// ---------------------------------------------------------------------------

#include <cctype>
#include <string>
#include <vector>

namespace uno {

struct Card {
    char color = 0;    // 'R' / 'Y' / 'G' / 'B'，万能牌为 'W'
    std::string face;  // "0".."9" / "SK" / "RV" / "D2" / "W" / "W4"

    bool valid() const { return color != 0 && !face.empty(); }
    bool wild() const { return color == 'W'; }
    std::string text() const {
        return wild() ? face : std::string(1, color) + face;
    }
};

inline Card parse_card(const std::string& text) {
    Card card;
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            t += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (t == "W") {
        card.color = 'W';
        card.face = "W";
        return card;
    }
    if (t == "W4") {
        card.color = 'W';
        card.face = "W4";
        return card;
    }
    if (t.size() < 2) return card;

    const char color = t[0];
    if (color != 'R' && color != 'Y' && color != 'G' && color != 'B')
        return card;

    const std::string face = t.substr(1);
    const bool number = face.size() == 1 &&
                        std::isdigit(static_cast<unsigned char>(face[0]));
    if (!number && face != "SK" && face != "RV" && face != "D2")
        return card;

    card.color = color;
    card.face = face;
    return card;
}

// "R5,R7,GD2" -> {"R5", "R7", "GD2"}
inline std::vector<std::string> split_card_list(const std::string& text) {
    std::vector<std::string> cards;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            if (!current.empty()) cards.push_back(current);
            current.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            current += c;
        }
    }
    if (!current.empty()) cards.push_back(current);
    return cards;
}

inline std::string join_cards(const std::vector<std::string>& cards) {
    std::string text;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (i != 0) text += ',';
        text += cards[i];
    }
    return text;
}

// 协议允许 red/blue/green/yellow，也接受单个字母。
inline char parse_color(const std::string& text) {
    std::string t;
    for (char c : text)
        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "r" || t == "red") return 'R';
    if (t == "y" || t == "yellow") return 'Y';
    if (t == "g" || t == "green") return 'G';
    if (t == "b" || t == "blue") return 'B';
    return 0;
}

inline std::string color_name(char color) {
    switch (color) {
    case 'R': return "red";
    case 'Y': return "yellow";
    case 'G': return "green";
    case 'B': return "blue";
    default: return "";
    }
}

inline int card_value(const Card& card) {
    if (!card.valid()) return 0;
    if (card.wild()) return 50; // W / W4
    if (card.face == "SK" || card.face == "RV" || card.face == "D2")
        return 20;
    return card.face[0] - '0';
}

inline int hand_value(const std::vector<std::string>& hand) {
    int total = 0;
    for (const std::string& name : hand) total += card_value(parse_card(name));
    return total;
}

} // 命名空间 uno
