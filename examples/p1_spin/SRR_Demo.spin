{{
==============================================================================
  SRR_Demo.spin
  Demonstrates Propeller P1 Multi-Cog SRR Message Passing
==============================================================================
  - Cog 0: Client issuing periodic synchronous requests & pulses
  - Cog 1: Server Cog handling requests (ADC/Sensor mock) and replying
==============================================================================
}}

CON
  _clkmode = xtal1 + pll16x
  _xinfreq = 5_000_000

OBJ
  srr : "SRR_HubMailbox"

VAR
  long  ServerStack[64]
  byte  LockId

PUB Main | st, reply[16]
  ' Allocate one of the 8 Propeller hardware locks
  LockId := locknew
  if LockId < 0
    abort

  ' Initialize the Hub RAM Mailbox
  srr.Init(LockId)

  ' Start Server in Cog 1
  cognew(RunServer(1), @ServerStack)
  waitcnt(cnt + clkfreq / 10) ' 100ms pause

  ' Cog 0 (Client) sends synchronous request to Cog 1 (Server)
  ' Request: "PING", expect "PONG"
  bytefill(@reply, 0, 16)
  st := srr.SendRequest(1, 1, 1, string("PING"), 4, @reply, 16, clkfreq)

  ' Send an asynchronous alarm pulse
  srr.SendPulse(1, 10, 42)

  repeat
    waitcnt(cnt + clkfreq)

PRI RunServer(MyCogId) | tid, svc, op, len, req_buf[16]
{{ Server task running in Cog 1 }}
  repeat
    tid := srr.Receive(MyCogId, @svc, @op, @req_buf, 16, @len)
    if tid > 0
      ' Service ID 1, Op 1: Echo / Ping service
      if svc == 1 and op == 1
        srr.Reply(MyCogId, string("PONG"), 4)
      else
        srr.Reply(MyCogId, string("OK"), 2)
    waitcnt(cnt + (clkfreq / 1000)) ' 1ms poll delay
