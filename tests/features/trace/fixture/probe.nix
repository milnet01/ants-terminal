# ANTS-5734 ship 3 (nix): a definition and a call of shipThreeBatchThree.
{
  shipThreeBatchThree = x: x;
  nixCaller = shipThreeBatchThree 1;
}
