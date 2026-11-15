package gui

import (
	"errors"
	"reflect"
	"testing"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// stoppingClient records the update calls and is disconnected once stopped;
// prepare is PrepareUpdate's answer.
type stoppingClient struct {
	*syncd.Mock
	calls   []string
	stopped bool
	prepare error
}

func (c *stoppingClient) PrepareUpdate(string) error {
	c.calls = append(c.calls, "prepare")
	return c.prepare
}

func (c *stoppingClient) Shutdown() error {
	c.calls = append(c.calls, "shutdown")
	c.stopped = true
	return nil
}

func (c *stoppingClient) StartAgain() { c.calls = append(c.calls, "start") }

func (c *stoppingClient) Snapshot() syncd.Snapshot {
	return syncd.Snapshot{Daemon: syncd.Daemon{Connected: !c.stopped}}
}

// TestHandOverStopsSyncDFirst: setup runs only once SyncD, held at the
// update safe point, has stopped (setup would otherwise wait out its limit
// against that hold and fail); a postponed update starts nothing, and a
// setup that refused has SyncD started again.
func TestHandOverStopsSyncDFirst(t *testing.T) {
	refused := errors.New("refused")
	for _, c := range []struct {
		name    string
		prepare error
		setup   error
		want    []string
		err     error
	}{
		{"installed", nil, errSetupDone, []string{"prepare", "shutdown", "setup"}, errSetupDone},
		{"setup refused", nil, refused, []string{"prepare", "shutdown", "setup", "start"}, refused},
		{"postponed", syncd.ErrBusy, nil, []string{"prepare"}, syncd.ErrBusy},
	} {
		t.Run(c.name, func(t *testing.T) {
			cl := &stoppingClient{Mock: syncd.NewMock(false, nil), prepare: c.prepare}
			err := handOver(cl, "0.1.3", time.Millisecond, func() error {
				if !cl.stopped {
					t.Error("setup started while SyncD still ran")
				}
				cl.calls = append(cl.calls, "setup")
				return c.setup
			})
			if !errors.Is(err, c.err) {
				t.Fatalf("handOver = %v, want %v", err, c.err)
			}
			if !reflect.DeepEqual(cl.calls, c.want) {
				t.Fatalf("calls %v, want %v", cl.calls, c.want)
			}
		})
	}
}
