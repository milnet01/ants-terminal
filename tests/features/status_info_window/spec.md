# ANTS-5759 — a status-bar pill's tooltip in a window

A tooltip can be too large for a screen magnifier. A click on a status-bar
pill that has no click action of its own opens a window showing the
pill's tooltip text, with an OK button. `src/statusinfowindow.h` holds it.

| Case | Asserts |
|---|---|
| `ClickShowsTheTooltipAndFollowsIt` | A pill shows the pointing-hand cursor. A left click opens one window titled as attached and showing the tooltip; a second click opens no second window. A tooltip change updates the open window, and an emptied tooltip closes it. |
| `EmptyTooltipAndRightClickOpenNothing` | A pill with no tooltip opens nothing on a click, and a right click opens nothing. |
| `WindowFitsTheText` | For a twelve-line tooltip, the window's text area is at least as large as the text, so none of it is hidden. |

**Would break this:** a click that opens nothing, a window that keeps the
text it opened with, or a window sized smaller than its text.

Which pills get the window is a user decision of 2026-10-16: only those
with no click action. The attach calls in `src/claudestatuswidgets.cpp` and
`src/mainwindow.cpp` are not asserted here.
