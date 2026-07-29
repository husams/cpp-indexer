#include <cstdint>

struct AccountState {
  std::int32_t balance = 100;
};

AccountState open_account() { return {}; }

int peer_balance() { return open_account().balance; }
