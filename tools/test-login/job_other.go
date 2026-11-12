//go:build !windows

package main

// ownChildren has nothing to do where the paired daemon cannot run.
func ownChildren() {}
