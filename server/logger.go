// Package main — leveled logging for the relay.
//
// Three levels (debug < info < error); messages below the configured
// level are dropped. Everything goes to stdout (container-friendly).
package main

import (
        "fmt"
        "log"
        "os"
        "strings"
)

// logLevel is a numeric log threshold: messages below the level are dropped.
type logLevel int

const (
        levelDebug logLevel = iota
        levelInfo
        levelError
)

// parseLogLevel parses the -log / ZZ_LOG_LEVEL value.
func parseLogLevel(s string) (logLevel, error) {
        switch strings.ToLower(strings.TrimSpace(s)) {
        case "debug":
                return levelDebug, nil
        case "info":
                return levelInfo, nil
        case "error":
                return levelError, nil
        }
        return levelInfo, fmt.Errorf("invalid log level %q (want debug, info or error)", s)
}

// relayLogger is a leveled wrapper around log.Logger. Safe for
// concurrent use (log.Logger locks internally).
type relayLogger struct {
        inner *log.Logger
        level logLevel
}

func newRelayLogger(level logLevel) *relayLogger {
        return &relayLogger{
                inner: log.New(os.Stdout, "", log.LstdFlags|log.Lmicroseconds),
                level: level,
        }
}

func (l *relayLogger) Debugf(format string, args ...any) { l.log(levelDebug, format, args...) }
func (l *relayLogger) Infof(format string, args ...any)  { l.log(levelInfo, format, args...) }
func (l *relayLogger) Errorf(format string, args ...any) { l.log(levelError, format, args...) }

func (l *relayLogger) log(lv logLevel, format string, args ...any) {
        if lv < l.level {
                return
        }
        l.inner.Printf(format, args...)
}
