struct AccountState {
  int balance = 0;
};

AccountState open_account() {
  return {};
}

int main() {
  const AccountState state = open_account();
  return state.balance;
}
