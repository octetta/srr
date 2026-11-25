%%%-------------------------------------------------------------------
%%% @doc SRR Client GenServer for BEAM (Erlang/OTP)
%%%
%%% Maps Erlang gen_server:call/3 onto synchronous SRR transactions,
%%% and gen_server:cast/2 onto asynchronous SRR pulses.
%%%
%%% Connects directly to local SRR Unix Domain Socket endpoints.
%%%-------------------------------------------------------------------
-module(srr_client).
-behaviour(gen_server).

%% API
-export([start_link/1, start_link/2, stop/1]).
-export([call/2, call/3, pulse/3]).

%% gen_server callbacks
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-define(SRR_MAGIC,   16#53525231). % "SRR1"
-define(SRR_VERSION, 1).

-define(SRR_MSG_HELLO,    1).
-define(SRR_MSG_REQUEST,  2).
-define(SRR_MSG_REPLY,    3).
-define(SRR_MSG_ERROR,    4).
-define(SRR_MSG_PULSE,    5).
-define(SRR_MSG_GOODBYE,  7).

-record(state, {
    socket           :: gen_tcp:socket(),
    service_path     :: string(),
    next_trans_id = 1 :: non_neg_integer(),
    pending = #{}    :: #{non_neg_integer() => {from(), timer:tref() | undefined}}
}).

-type from() :: {pid(), tag()}.
-type tag() :: term().

%%%===================================================================
%%% API Functions
%%%===================================================================

-spec start_link(ServiceName :: string() | binary()) -> {ok, pid()} | {error, term()}.
start_link(ServiceName) ->
    start_link(ServiceName, []).

-spec start_link(ServiceName :: string() | binary(), Options :: list()) -> {ok, pid()} | {error, term()}.
start_link(ServiceName, Options) ->
    gen_server:start_link(?MODULE, [ServiceName], Options).

-spec stop(Pid :: pid()) -> ok.
stop(Pid) ->
    gen_server:stop(Pid).

%% Synchronous Send / Receive / Reply
-spec call(Pid :: pid(), Request :: binary()) -> {ok, binary()} | {error, term()}.
call(Pid, Request) ->
    call(Pid, Request, 5000).

-spec call(Pid :: pid(), Request :: binary(), TimeoutMs :: non_neg_integer()) ->
    {ok, binary()} | {error, term()}.
call(Pid, Request, TimeoutMs) when is_binary(Request), is_integer(TimeoutMs) ->
    gen_server:call(Pid, {send_request, Request, TimeoutMs}, TimeoutMs + 1000).

%% One-way pulse notification
-spec pulse(Pid :: pid(), Code :: non_neg_integer(), Value :: non_neg_integer()) -> ok.
pulse(Pid, Code, Value) when is_integer(Code), is_integer(Value) ->
    gen_server:cast(Pid, {send_pulse, Code, Value}).

%%%===================================================================
%%% gen_server callbacks
%%%===================================================================

init([ServiceName]) ->
    Path = resolve_service_path(ServiceName),
    case gen_tcp:connect({local, Path}, 0, [binary, {active, true}, {packet, 0}]) of
        {ok, Socket} ->
            {ok, #state{socket = Socket, service_path = Path}};
        {error, Reason} ->
            {stop, {connect_failed, Reason, Path}}
    end.

handle_call({send_request, Payload, _TimeoutMs}, From, #state{socket = Socket, next_trans_id = Tid, pending = Pending} = State) ->
    PayloadLen = byte_size(Payload),
    Frame = <<?SRR_MAGIC:32/little, ?SRR_VERSION:16/little, ?SRR_MSG_REQUEST:16/little,
              0:32/little, Tid:64/little, PayloadLen:32/little, Payload/binary>>,
    case gen_tcp:send(Socket, Frame) of
        ok ->
            NewPending = maps:put(Tid, From, Pending),
            {noreply, State#state{next_trans_id = Tid + 1, pending = NewPending}};
        {error, Reason} ->
            {reply, {error, {send_error, Reason}}, State}
    end;

handle_call(_Request, _From, State) ->
    {reply, {error, unknown_call}, State}.

handle_cast({send_pulse, Code, Value}, #state{socket = Socket} = State) ->
    Payload = <<Code:32/little, 0:32/little, Value:64/little>>,
    PayloadLen = byte_size(Payload),
    Frame = <<?SRR_MAGIC:32/little, ?SRR_VERSION:16/little, ?SRR_MSG_PULSE:16/little,
              0:32/little, 0:64/little, PayloadLen:32/little, Payload/binary>>,
    _ = gen_tcp:send(Socket, Frame),
    {noreply, State};

handle_cast(_Msg, State) ->
    {noreply, State}.

handle_info({tcp, _Socket, <<?SRR_MAGIC:32/little, ?SRR_VERSION:16/little, MsgType:16/little,
                             _Flags:32/little, TransId:64/little, PayloadLen:32/little,
                             Rest/binary>>}, State) ->
    handle_frame(MsgType, TransId, PayloadLen, Rest, State);

handle_info({tcp_closed, _Socket}, State) ->
    %% Inform all pending callers of disconnect
    maps:foreach(fun(_Tid, From) -> gen_server:reply(From, {error, disconnected}) end, State#state.pending),
    {stop, normal, State#state{pending = #{}}};

handle_info({tcp_error, _Socket, Reason}, State) ->
    maps:foreach(fun(_Tid, From) -> gen_server:reply(From, {error, {tcp_error, Reason}}) end, State#state.pending),
    {stop, Reason, State#state{pending = #{}}};

handle_info(_Info, State) ->
    {noreply, State}.

terminate(_Reason, #state{socket = Socket}) ->
    if Socket =/= undefined ->
        Goodbye = <<?SRR_MAGIC:32/little, ?SRR_VERSION:16/little, ?SRR_MSG_GOODBYE:16/little,
                    0:32/little, 0:64/little, 0:32/little>>,
        _ = gen_tcp:send(Socket, Goodbye),
        gen_tcp:close(Socket);
       true -> ok
    end,
    ok.

code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

%%%===================================================================
%%% Internal functions
%%%===================================================================

handle_frame(?SRR_MSG_REPLY, TransId, PayloadLen, Rest, #state{pending = Pending} = State) ->
    <<Payload:PayloadLen/binary, _/binary>> = Rest,
    case maps:take(TransId, Pending) of
        {From, NewPending} ->
            gen_server:reply(From, {ok, Payload}),
            {noreply, State#state{pending = NewPending}};
        error ->
            {noreply, State}
    end;

handle_frame(?SRR_MSG_ERROR, TransId, _PayloadLen, <<ErrorCode:32/little, _/binary>>, #state{pending = Pending} = State) ->
    case maps:take(TransId, Pending) of
        {From, NewPending} ->
            gen_server:reply(From, {error, decode_error(ErrorCode)}),
            {noreply, State#state{pending = NewPending}};
        error ->
            {noreply, State}
    end;

handle_frame(_MsgType, _TransId, _PayloadLen, _Rest, State) ->
    {noreply, State}.

decode_error(1) -> timeout;
decode_error(2) -> cancelled;
decode_error(3) -> disconnected;
decode_error(4) -> server_died;
decode_error(5) -> closed;
decode_error(6) -> queue_full;
decode_error(7) -> too_large;
decode_error(8) -> protocol_error;
decode_error(Code) -> {unknown_error, Code}.

resolve_service_path(Name) when is_binary(Name) ->
    resolve_service_path(binary_to_list(Name));
resolve_service_path(Name) when is_list(Name) ->
    case lists:member($/, Name) of
        true  -> Name;
        false -> "/tmp/srr_" ++ Name ++ ".sock"
    end.
