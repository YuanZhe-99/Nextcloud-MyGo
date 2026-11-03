//go:build !windows

package gui

import "time"

// idleTime is unknown off Windows: never idle, so "When idle" never
// installs there.
func idleTime() time.Duration { return 0 }

// lightTaskbar: the other systems tint the tray glyph themselves.
func lightTaskbar() bool { return false }
