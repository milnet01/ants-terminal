"""Colours a style gives in short hex form reach every formatter in full."""
from pygments.formatters import LatexFormatter
from pygments.style import Style
from pygments.token import Keyword, Name, String


class ShortHexStyle(Style):
    styles = {
        Keyword: '#0a0',
        Name: 'bg:#123 #f80',
        String: 'border:#09c',
    }


def test_foreground():
    assert ShortHexStyle.style_for_token(Keyword)['color'] == '00aa00'


def test_foreground_and_background():
    style = ShortHexStyle.style_for_token(Name)
    assert style['color'] == 'ff8800'
    assert style['bgcolor'] == '112233'


def test_border():
    assert ShortHexStyle.style_for_token(String)['border'] == '0099cc'


def test_latex_colour():
    defs = LatexFormatter(style=ShortHexStyle).get_style_defs()
    assert r'\textcolor[rgb]{0.00,0.67,0.00}' in defs
