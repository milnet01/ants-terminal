// ANTS-5734 ship 3 (qmljs): a definition and a call of shipThreeProbe.
import QtQuick

Item {
    function shipThreeProbe() {}
    function qmlCaller() { shipThreeProbe() }
}
