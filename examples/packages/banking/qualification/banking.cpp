#include <cstdint>

struct AccountState {
  std::int32_t balance = 0;
};

AccountState open_account() { return {}; }

AccountState deposit(AccountState state) {
  ++state.balance;
  return state;
}

AccountState withdraw(AccountState state) {
  --state.balance;
  return state;
}

int audit(AccountState state) { return state.balance; }

int main() {
  AccountState state = open_account();
  state = deposit(state);
  state = withdraw(state);
  return audit(state);
}
