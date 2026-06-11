// Feature-conformance test for spec.md (ANTS-2098).
//
// RUNTIME RENDER test — the predecessor (ANTS-1147 / 0.7.32) was a
// source-grep test that only checked the close-button QSS *text* existed.
// It stayed green for the entire life of a dead feature: Qt6's stylesheet
// engine cannot load `image: url("data:...")` (its loader is QPixmap(path),
// which has no data-scheme support), so the data-URI × rendered nothing on
// every theme. A grep test cannot catch "renders nothing". This test
// instead instantiates a ColoredTabBar offscreen, renders it, and asserts
// the × glyph produces visible pixels — plus the close-button wiring.
//
// Runs in the test_chrome GUI bundle, whose bundle_main_gui.cpp sets
// QT_QPA_PLATFORM=offscreen and constructs the QApplication.

#include "coloredtabbar.h"

#include <QColor>
#include <QImage>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <Qt>

#include <cstdio>
#include <string>
#include <gtest/gtest.h>

#include "../../_support/srcgrep.h"

// Regression guard (I4) reads the QSS source to forbid the dead data-URI
// rule from creeping back in.
#ifndef SRC_THEMEDSTYLESHEET_CPP_PATH
#  error "SRC_THEMEDSTYLESHEET_CPP_PATH compile definition required"
#endif

namespace {

// Count near-white pixels — the × glyph is rendered pure white in this
// test, against a dark tab-bar fill, so a white-pixel count isolates it.
int whitePixels(const QImage &img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qRed(c) > 200 && qGreen(c) > 200 && qBlue(c) > 200) ++n;
        }
    return n;
}

QImage renderBar(ColoredTabBar &bar) {
    QImage img(bar.size(), QImage::Format_ARGB32);
    img.fill(Qt::black);
    bar.render(&img);
    return img;
}

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n", __FUNCTION__,    \
                         msg, __LINE__);                                        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

}  // namespace

TEST(TabCloseButtonVisible, Main) {
    int failures = 0;

    ColoredTabBar bar;
    bar.setTabsClosable(true);
    bar.setBackgroundFill(QColor("#101010"));  // dark fill for white-on-dark
    bar.addTab("Tab one");
    bar.addTab("Tab two");
    bar.resize(360, 40);

    const auto side = static_cast<QTabBar::ButtonPosition>(bar.style()->styleHint(
        QStyle::SH_TabBar_CloseButtonPosition, nullptr, &bar));

    // I1 — every tab carries OUR themed close button, not Qt's built-in
    // (non-themable) CloseButton.
    for (int i = 0; i < bar.count(); ++i) {
        auto *btn = qobject_cast<QToolButton *>(bar.tabButton(i, side));
        CHECK(btn != nullptr, "tab has a QToolButton close button");
        CHECK(btn && btn->objectName() == QLatin1String("antsTabClose"),
              "close button is ours (objectName antsTabClose)");
        CHECK(btn && !btn->accessibleName().isEmpty(),
              "close button keeps an accessible name for AT-SPI/Orca");
    }

    // I2 — the × actually RENDERS. Diff method: render once with the glyph
    // coloured to MATCH the dark fill (invisible) and once white; the
    // delta in white pixels is the glyph alone (tab text/borders cancel).
    bar.setCloseGlyphColors(QColor("#101010"), QColor("#101010"),
                            QColor("#e74856"));
    const int hidden = whitePixels(renderBar(bar));
    bar.setCloseGlyphColors(QColor("#ffffff"), QColor("#ffffff"),
                            QColor("#e74856"));
    const int shown = whitePixels(renderBar(bar));
    CHECK(shown > hidden,
          "white × glyph adds visible pixels when tinted (it renders)");

    // I3 — clicking the button requests closing ITS tab, resolved at the
    // live index (tab moves/closes reshuffle indices).
    int requested = -1;
    QObject::connect(&bar, &QTabBar::tabCloseRequested, &bar,
                     [&](int idx) { requested = idx; });
    if (auto *btn = qobject_cast<QToolButton *>(bar.tabButton(1, side))) {
        btn->click();
        CHECK(requested == 1,
              "click emits tabCloseRequested for the button's own tab");
    }

    // I4 (regression) — the dead data-URI close-button rule must NOT
    // return. Qt6 QSS can't load a `data:` image (ANTS-2098); reintroducing
    // it silently re-breaks the glyph that this test now guards at runtime.
    const std::string ss = ants_test::slurpFile(SRC_THEMEDSTYLESHEET_CPP_PATH);
    CHECK(ss.find("data:image/svg+xml") == std::string::npos,
          "no data-URI image rule reintroduced in themedstylesheet.cpp");

    if (failures == 0) {
        std::printf("tab_close_button_visible: glyph renders + wiring ok\n");
        return;
    }
    std::fprintf(stderr, "tab_close_button_visible: %d failure(s)\n", failures);
    FAIL();
}
