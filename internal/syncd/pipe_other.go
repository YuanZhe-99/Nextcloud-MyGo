//go:build !windows

package syncd

import (
	"context"
	"io"
)

// dialPipe refuses platforms whose authenticated local transport is not implemented.
func dialPipe(context.Context, string, string) (io.ReadWriteCloser, error) { return nil, ErrRefused }

// startDaemon refuses unsupported native process transports.
func startDaemon(string, string) error { return ErrRefused }
