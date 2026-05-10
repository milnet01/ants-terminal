// ANTS-1217 — bundle main for Qt::Core-only GoogleTest bundles.
// Used by test_core (grep-style audit-rule tests + helper-CLI tests). No GUI
// symbols are pulled in, so QApplication / Qt6::Widgets is not required.
#include <gtest/gtest.h>
#include <QCoreApplication>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
