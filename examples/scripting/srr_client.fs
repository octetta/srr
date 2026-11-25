# ==============================================================================
# SRR Gforth Client Example
# Demonstrates communicating with SRR using Forth stack operations.
# Gforth can communicate via external C library bindings or srr_cli pipes.
# ==============================================================================

\ Define string helper
: s"s ( "ccc<quote>" -- c-addr u ) [char] " parse ;

\ Synchronous SRR request execution via srr_cli pipe
: srr-query ( c-addr u -- )
    s" ./tools/srr_cli --sim req 1 1" system
;

\ Synchronous ping
: srr-ping ( -- )
    s" ./tools/srr_cli --sim ping" system
;

\ Asynchronous pulse
: srr-pulse ( -- )
    s" ./tools/srr_cli --sim pulse 10 1 FORTH_TRIGGER" system
;

cr .( === Gforth SRR Integration Example === ) cr
.( Testing ping... ) cr
srr-ping

.( Testing device query... ) cr
srr-query

.( Emitting pulse... ) cr
srr-pulse
cr .( Gforth example finished. ) cr
bye
