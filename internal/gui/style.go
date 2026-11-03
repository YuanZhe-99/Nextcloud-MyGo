package gui

import (
	"github.com/egoist/mygo/ui"
)

// palette is the design's colours (design/README.md, "Tokens") for one
// appearance. Views take it from pal, never from literals of their own.
type palette struct {
	base, pane, border, subtle, hover, text, muted ui.Color
	// accent fills primary buttons with onAccent on it; accentText is the
	// accent as words and links; tint is the accent's pale background.
	accent, onAccent, accentText, tint, tintBorder ui.Color
	success                                        ui.Color
	warn, warnBg, warnBorder, onWarn               ui.Color
	danger, dangerBg, dangerBorder                 ui.Color
	scrim                                          ui.Color
}

var (
	lightPal = palette{
		base: ui.Hex("#E9EEF3"), pane: ui.Hex("#FFFFFF"), border: ui.Hex("#DCE2E8"), subtle: ui.Hex("#F6F8FA"), hover: ui.Hex("#EEF2F6"),
		text: ui.Hex("#1B1D21"), muted: ui.Hex("#4A5260"),
		accent: ui.Hex("#0072B5"), onAccent: ui.Hex("#FFFFFF"), accentText: ui.Hex("#0069A6"), tint: ui.Hex("#E3F1FA"), tintBorder: ui.Hex("#9CC9E6"),
		success: ui.Hex("#0F7B3F"),
		warn:    ui.Hex("#8A4B00"), warnBg: ui.Hex("#FFF6E5"), warnBorder: ui.Hex("#F1D49B"), onWarn: ui.Hex("#FFFFFF"),
		danger: ui.Hex("#B42318"), dangerBg: ui.Hex("#FDF4F3"), dangerBorder: ui.Hex("#F0B8B2"),
		scrim: ui.RGBA(20, 26, 34, 0.42),
	}
	darkPal = palette{
		base: ui.Hex("#1A1C21"), pane: ui.Hex("#23262C"), border: ui.Hex("#33363E"), subtle: ui.Hex("#2A2D34"), hover: ui.Hex("#30333B"),
		text: ui.Hex("#EDEFF2"), muted: ui.Hex("#A9AFBA"),
		accent: ui.Hex("#4DAEE8"), onAccent: ui.Hex("#06131C"), accentText: ui.Hex("#7CC6F2"), tint: ui.Hex("#173A52"), tintBorder: ui.Hex("#2B5B7C"),
		success: ui.Hex("#4CC38A"),
		warn:    ui.Hex("#F0B65A"), warnBg: ui.Hex("#352812"), warnBorder: ui.Hex("#6A4D1C"), onWarn: ui.Hex("#231703"),
		danger: ui.Hex("#F2877B"), dangerBg: ui.Hex("#3A1F1C"), dangerBorder: ui.Hex("#6E3029"),
		scrim: ui.RGBA(0, 0, 0, 0.55),
	}
)

// pal is the palette of the appearance the window shows.
func pal(c *ui.Context) *palette {
	if c.Theme().Dark {
		return &darkPal
	}
	return &lightPal
}

// applyTheme gives MyGo's own widgets (switches, check boxes, selects,
// focus rings) the design's accent and ground.
func applyTheme(c *ui.Context) {
	p := pal(c)
	t := *c.Theme()
	t.Accent, t.AccentText, t.Background, t.Text, t.TextMuted, t.Border = p.accent, p.onAccent, p.pane, p.text, p.muted, p.border
	t.Font = fontFamily
	c.SetTheme(&t)
}

// Sizes from the design (design/README.md): type, radii and the window's
// margin around its pane.
const (
	fontFamily  = "Segoe UI Variable Text"
	fontSmall   = 12
	fontBody    = 14
	fontSection = 15
	fontTitle   = 24
	radiusCtl   = 6
	radiusCard  = 8
	radiusDlg   = 10
	sidebarW    = 232
	titleBarH   = 40
)

// Icons: line drawings on a 24-point grid, drawn in the text colour.
func svg(body string) *ui.SVG {
	return ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">` + body + `</svg>`))
}

var (
	icHome     = svg(`<path d="M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z"/>`)
	icFolder   = svg(`<path d="M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/>`)
	icGlobe    = svg(`<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18"/>`)
	icPulse    = svg(`<path d="M3 12h4l3-7 4 14 3-7h4"/>`)
	icGear     = svg(`<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M4.9 19.1L7 17M17 7l2.1-2.1"/>`)
	icPause    = svg(`<path d="M8 5v14M16 5v14"/>`)
	icPlay     = svg(`<path d="M7 5l12 7-12 7z"/>`)
	icRefresh  = svg(`<path d="M20 12a8 8 0 1 1-2.3-5.7M20 4v5h-5"/>`)
	icPlus     = svg(`<path d="M12 5v14M5 12h14"/>`)
	icCloudUp  = svg(`<path d="M7 18a5 5 0 0 1-.6-10A6 6 0 0 1 18 9a4.5 4.5 0 0 1-.5 9z"/><path d="M12 16v-5M9.5 13.5L12 11l2.5 2.5"/>`)
	icCloud    = svg(`<path d="M7 18a5 5 0 0 1-.6-10A6 6 0 0 1 18 9a4.5 4.5 0 0 1-.5 9z"/>`)
	icCheck    = svg(`<path d="M5 12l5 5 9-10"/>`)
	icHouse    = svg(`<path d="M3 10l9-6 9 6M5 9v11h14V9"/><path d="M10 20v-5h4v5"/>`)
	icShield   = svg(`<path d="M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6z"/>`)
	icBlock    = svg(`<circle cx="12" cy="12" r="9"/><path d="M6 6l12 12"/>`)
	icClock    = svg(`<circle cx="12" cy="12" r="8"/><path d="M12 8v4l2.5 2"/>`)
	icWarn     = svg(`<path d="M12 3l10 18H2z"/><path d="M12 10v5M12 18v.5"/>`)
	icLock     = svg(`<rect x="5" y="10" width="14" height="10" rx="1.5"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>`)
	icFile     = svg(`<path d="M6 3h8l4 4v14H6z"/>`)
	icFiles    = svg(`<path d="M7 4h7l4 4v12H7z"/><path d="M4 8v12h11"/>`)
	icUp       = svg(`<path d="M12 19V7M7 11l5-5 5 5"/>`)
	icDown     = svg(`<path d="M12 5v12M7 13l5 5 5-5"/>`)
	icTrash    = svg(`<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>`)
	icPower    = svg(`<path d="M12 3v9M6.3 7.3a8 8 0 1 0 11.4 0"/>`)
	icTray     = svg(`<rect x="3" y="5" width="18" height="15" rx="2"/><path d="M3 17h18M7 14h2M11 14h2"/>`)
	icWinX     = svg(`<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M14 9l4 4M18 9l-4 4"/>`)
	icBell     = svg(`<path d="M6 16V11a6 6 0 0 1 12 0v5l2 2H4z"/><path d="M10 20a2 2 0 0 0 4 0"/>`)
	icDownload = svg(`<path d="M12 4v11M7 10l5 5 5-5M5 20h14"/>`)
	icDoc      = svg(`<path d="M6 3h8l4 4v14H6z"/><path d="M9 12h6M9 16h6"/>`)
	icSearch   = svg(`<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>`)
	icMore     = svg(`<circle cx="5" cy="12" r="1" fill="currentColor"/><circle cx="12" cy="12" r="1" fill="currentColor"/><circle cx="19" cy="12" r="1" fill="currentColor"/>`)
	icClose    = svg(`<path d="M6 6l12 12M18 6L6 18"/>`)
	icChevron  = svg(`<path d="M7 10l5 5 5-5"/>`)
	icMonitor  = svg(`<rect x="3" y="4" width="18" height="12" rx="1.5"/><path d="M8 20h8M12 16v4"/>`)
	icBrowser  = svg(`<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 8h18"/>`)
	icLanguage = svg(`<path d="M4 5h9M8.5 3v2M6 5c1 4 4 7 7 8M11 5c-1 4-4 7-7 8M13 21l4-9 4 9M14.5 18h5"/>`)
	icContrast = svg(`<circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" fill="currentColor"/>`)
	icInfo     = svg(`<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8v.5"/>`)
	icLog      = svg(`<path d="M6 3h8l4 4v14H6z"/><path d="M9 11h6M9 14h6M9 17h4"/>`)
	icArchive  = svg(`<rect x="3" y="4" width="18" height="5" rx="1"/><path d="M5 9v10h14V9M10 13h4"/>`)

	// The mark: a placeholder for the Nextcloud icon (design/README.md),
	// three rings drawn in white on the accent.
	icMark = ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 16" fill="none" stroke="currentColor" stroke-width="3"><circle cx="5" cy="8" r="3.5"/><circle cx="16" cy="8" r="5.5"/><circle cx="27" cy="8" r="3.5"/></svg>`))

	// Windows 11's caption glyphs, 10 points with lines of one.
	winMin   = ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><path d="M0 5h10" stroke="currentColor"/></svg>`))
	winMax   = ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect x="0.5" y="0.5" width="9" height="9" fill="none" stroke="currentColor"/></svg>`))
	winRest  = ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10" fill="none" stroke="currentColor"><rect x="0.5" y="2.5" width="7" height="7"/><path d="M2.5 2.5V0.5h7v7h-2"/></svg>`))
	winClose = ui.MustParseSVG([]byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><path d="M0 0l10 10M10 0L0 10" stroke="currentColor"/></svg>`))
)

// mark is the app's mark on its accent tile, size points square.
func mark(c *ui.Context, size float32) {
	p := pal(c)
	ui.Box(c).Size(size, size).Shrink(0).Radius(size * 0.22).Background(p.accent).Center().Children(func() {
		ui.Icon(c, icMark).FontSize(size * 0.36).TextColor(p.onAccent)
	})
}

// tile is an icon on a square tile, as Settings' rows and the hero start.
func tile(c *ui.Context, icon *ui.SVG, size float32, bg, fg ui.Color) {
	ui.Box(c).Size(size, size).Shrink(0).Radius(size * 0.22).Background(bg).Center().Children(func() {
		ui.Icon(c, icon).FontSize(size * 0.5).TextColor(fg)
	})
}

// dot is a status dot; hollow for a state that waits on something
// outside the app (signing in).
func dot(c *ui.Context, col ui.Color, hollow bool) {
	d := ui.Box(c).Size(8, 8).Shrink(0).Radius(4)
	if hollow {
		d.Border(2, col)
	} else {
		d.Background(col)
	}
}

// pill is a short label in a rounded pill.
func pill(c *ui.Context, text string, bg, fg ui.Color) {
	ui.Box(c).Padding(1, 8).Radius(10).Background(bg).Shrink(0).Children(func() {
		ui.Text(c, text).FontSize(fontSmall).FontWeight(600).TextColor(fg).NoWrap()
	})
}

// button is the design's secondary button: an outline on the pane.
// Returns: the button, whose Clicked tells a click.
func button(c *ui.Context, key, label string, icon *ui.SVG) ui.Element {
	p := pal(c)
	b := ui.ButtonBase(c.Key(key)).Height(32).Padding(0, 14).Radius(radiusCtl).Border(1, p.border).Background(p.pane).Label(label)
	if b.Hovered() {
		b.Background(p.hover)
	}
	b.Children(func() {
		ui.Row(c).Gap(8).Children(func() {
			if icon != nil {
				ui.Icon(c, icon).FontSize(14).TextColor(p.text)
			}
			ui.Text(c, label).TextColor(p.text).NoWrap()
		})
	})
	return b
}

// primary is the design's primary button, filled with the accent.
func primary(c *ui.Context, key, label string, icon *ui.SVG) ui.Element {
	p := pal(c)
	return filled(c, key, label, icon, p.accent, p.onAccent)
}

// filled is a button filled with bg, its label in fg (the warning colour's
// Review, the accent's primary).
func filled(c *ui.Context, key, label string, icon *ui.SVG, bg, fg ui.Color) ui.Element {
	b := ui.ButtonBase(c.Key(key)).Height(32).Padding(0, 14).Radius(radiusCtl).Background(bg).Label(label)
	if b.Hovered() {
		b.Opacity(0.9)
	}
	if b.IsDisabled() {
		b.Opacity(0.45)
	}
	b.Children(func() {
		ui.Row(c).Gap(8).Children(func() {
			if icon != nil {
				ui.Icon(c, icon).FontSize(14).TextColor(fg)
			}
			ui.Text(c, label).FontWeight(600).TextColor(fg).NoWrap()
		})
	})
	return b
}

// link is words in the accent that act as a button.
func link(c *ui.Context, key, label string) ui.Element {
	p := pal(c)
	b := ui.ButtonBase(c.Key(key)).Label(label)
	b.Children(func() {
		t := ui.Text(c, label).FontSize(13).TextColor(p.accentText)
		if b.Hovered() {
			t.Underline()
		}
	})
	return b
}

// card is a bordered box on the pane.
func card(c *ui.Context) ui.Element {
	p := pal(c)
	return ui.Column(c).Border(1, p.border).Radius(radiusCard)
}

// sectionTitle is a section's heading on a page.
func sectionTitle(c *ui.Context, text string) {
	ui.Text(c, text).FontSize(fontSection).FontWeight(600).Level(2)
}

// muted is secondary text.
func muted(c *ui.Context, text string, size float32) ui.Element {
	return ui.Text(c, text).FontSize(size).TextColor(pal(c).muted)
}
