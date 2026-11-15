// Package testprofile removes the disposable SyncD profiles that real-daemon tests create, and tells a
// file still held by a SyncD (a test failure) from one held by another process such as a virus scanner
// (logged, the profile is left behind). Only tests import it.
package testprofile
