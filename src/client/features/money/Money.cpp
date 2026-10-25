// /pay <nick> <amount>: money from this player to another one. The payment goes to that player's game, which adds it and
// says so; only then is it taken from this player. A payment nobody took (a game without /pay, the other player left)
// costs nothing, and one that was taken is never taken twice: a payment's id is answered once.
#include <cstdlib>
#include <iterator>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

enum : uint8_t { OP_PAY = 1, OP_TAKEN = 2, OP_REFUSED = 3 };
enum : uint8_t { WHY_NOT_READY = 1, WHY_TOO_RICH = 2 };
constexpr int32_t kMaxMoney = 999999999;  // the game's money counter shows nine digits
constexpr uint64_t kSlowMs = 10000;        // no answer this long: the sender is told (the payment still waits)

struct Payment {
    uint8_t to = 0;
    int32_t amount = 0;
    uint64_t sentMs = 0;
    bool toldSlow = false;
};
std::map<uint32_t, Payment> g_sent;  // ours, waiting for the answer
uint32_t g_nextId = 1;

int32_t& Money() { return PlayerInfos()[0].money; }

// what our player can still give: the money less the payments not answered yet
int64_t Spendable() {
    int64_t out = Money();
    for (auto& [id, p] : g_sent) out -= p.amount;
    return out;
}

void Answer(uint8_t to, uint8_t op, uint32_t id, int32_t amount) {
    Writer w(proto::G_MONEY);
    w.U8(op);
    w.U32(id);
    w.I32(amount);
    Net().SendTo(to, proto::CH_RELIABLE, w);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint32_t id = r.U32();
    int32_t amount = r.I32();
    if (!r.Ok() || from == Net().MyId()) return;
    auto& chat = ui::TheChat();
    if (op == OP_PAY) {
        if (amount <= 0) return;
        if (!core::IsPlayerReady()) return Answer(from, OP_REFUSED, id, WHY_NOT_READY);
        if ((int64_t)Money() + amount > kMaxMoney) return Answer(from, OP_REFUSED, id, WHY_TOO_RICH);
        Money() += amount;
        Answer(from, OP_TAKEN, id, amount);
        chat.AddSystem(Format(TR("%s перевёл вам $%d.", "%s sent you $%d."), Net().PeerName(from).c_str(), amount));
        LOGI("Money: $%d from player %d", amount, from);
        return;
    }
    auto it = g_sent.find(id);
    if (it == g_sent.end() || it->second.to != from) return;
    Payment p = it->second;
    g_sent.erase(it);
    std::string who = Net().PeerName(from);
    if (op == OP_TAKEN) {
        Money() -= p.amount;
        chat.AddSystem(Format(TR("Вы перевели $%d игроку %s.", "You sent $%d to %s."), p.amount, who.c_str()));
        LOGI("Money: $%d to player %d", p.amount, from);
    } else if (op == OP_REFUSED) {
        chat.AddError(amount == WHY_TOO_RICH
                          ? Format(TR("%s не может получить столько: у него будет больше $999 999 999. Деньги остались у вас.",
                                      "%s cannot take that much: they would have more than $999,999,999. You keep the money."),
                                   who.c_str())
                          : Format(TR("%s сейчас не в игре (загружается). Деньги остались у вас.",
                                      "%s is not in the game right now (loading). You keep the money."),
                                   who.c_str()));
        LOGI("Money: $%d to player %d refused (%d)", p.amount, from, amount);
    }
}

bool Pay(const std::vector<std::string>& args) {
    auto& chat = ui::TheChat();
    if (!Net().Online() || !core::IsPlayerReady()) return true;
    if (args.size() < 2) {
        chat.AddError(TR("Как переводить: /pay <ник> <сумма>, например /pay master 5000.",
                         "How to pay: /pay <nick> <amount>, for example /pay master 5000."));
        return true;
    }
    uint8_t to = Net().FindPeer(args[0]);
    if (to == 255 || to == Net().MyId()) {
        chat.AddError(Format(TR("Нет игрока «%s» (или под этот ник подходят несколько).",
                                "There is no player \"%s\" (or several fit the name)."), args[0].c_str()));
        return true;
    }
    const RemotePlayer* rp = Players::Instance().Get(to);
    if (!rp || !rp->hasState) {
        chat.AddError(Format(TR("%s ещё не в игре.", "%s is not in the game yet."), Net().PeerName(to).c_str()));
        return true;
    }
    std::string text = args[1];
    if (!text.empty() && text[0] == '$') text.erase(0, 1);
    char* end = nullptr;
    long long amount = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || amount <= 0 || amount > kMaxMoney) {
        chat.AddError(TR("Сумма — целое число долларов больше нуля.", "The amount is a whole number of dollars above zero."));
        return true;
    }
    if (amount > Spendable()) {
        chat.AddError(Format(TR("У вас нет $%lld.", "You do not have $%lld."), amount));
        return true;
    }
    uint32_t id = g_nextId++;
    g_sent[id] = {to, (int32_t)amount, NowMs(), false};
    Answer(to, OP_PAY, id, (int32_t)amount);
    LOGI("Money: paying $%lld to player %d (payment %u)", amount, to, id);
    return true;
}

class MoneyModule : public Module {
public:
    const char* Name() const override { return "Money"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_MONEY, &Handle); }
    void OnSessionEnd() override { g_sent.clear(); }
    void OnPeerLeave(uint8_t id) override {  // its answer will not come: nothing was taken
        for (auto it = g_sent.begin(); it != g_sent.end();) it = it->second.to == id ? g_sent.erase(it) : std::next(it);
    }
    void OnPostProcess() override {
        uint64_t now = NowMs();
        for (auto& [id, p] : g_sent)
            if (!p.toldSlow && now - p.sentMs > kSlowMs) {
                p.toldSlow = true;
                ui::TheChat().AddError(Format(TR("%s пока не ответил на перевод $%d. Деньги спишутся, только когда он их получит.",
                                                 "%s has not answered the payment of $%d yet. The money goes only once they get it."),
                                              Net().PeerName(p.to).c_str(), p.amount));
            }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        return cmd == "pay" && Pay(args);
    }
};

MoneyModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
