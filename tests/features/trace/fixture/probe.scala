// ANTS-5734 ship 3 (scala): a definition and a call of shipThreeBatchTwo.
object Probe {
  def shipThreeBatchTwo(): Unit = {}
  def scalaCaller(): Unit = shipThreeBatchTwo()
}
