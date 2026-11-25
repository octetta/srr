#!/usr/bin/env bash
# ==============================================================================
# SRR Shell Script Server Handler
# ==============================================================================
# Invoked by `srr_cli serve --exec ...` for each incoming request.
# Receives the request payload on stdin, outputs the reply on stdout.
# ==============================================================================

read -r REQUEST

case "$REQUEST" in
    "PING")
        echo "PONG from Bash SRR Server!"
        ;;
    "TIME")
        date -u +"%Y-%m-%dT%H:%M:%SZ"
        ;;
    "UNAME")
        uname -s -m
        ;;
    "DISK")
        df -h / | awk 'NR==2 {print $4}'
        ;;
    "CALC "*)
        EXPR="${REQUEST#CALC }"
        echo $(( EXPR )) 2>/dev/null || echo "ERR_INVALID_EXPR"
        ;;
    *)
        echo "ERR_UNKNOWN_CMD: $REQUEST"
        ;;
esac
