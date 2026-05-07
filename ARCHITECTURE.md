# mod_siprec — RFC 7866 / RFC 7865 implementation plan

mod_siprec exposes SIP-based session recording per RFC 7866
(SIPREC) for FreeSWITCH. The module is a Session Recording
Client (SRC): for each call leg the operator wants recorded, it
opens a parallel SIP INVITE to a configured Session Recording
Server (SRS), describes the original session in a multipart MIME
body (SDP for the media + XML for the metadata), and forks the
captured audio to the negotiated RTP endpoints.

## RFC mapping

| Concept              | RFC reference        | This implementation |
|----------------------|----------------------|---------------------|
| SRC INVITE           | RFC 7866 §6.1        | `siprec_send_invite()` (sofia-sip NUA) |
| Required SDP labels  | RFC 7866 §7.2 + §8.5 | `a=label:1`, `a=label:2` per stream |
| `a=sendonly` on SRC  | RFC 7866 §7.4        | media bug taps original RTP, never receives |
| Multipart body       | RFC 7866 §6.1.2      | `multipart/mixed`; SDP first, metadata second |
| Metadata XML         | RFC 7865             | `application/rs-metadata+xml` |
| Session lifecycle    | RFC 7866 §6.4        | INVITE on start, BYE on hangup, re-INVITE on pause/resume |
| Mid-call updates     | RFC 7866 §6.4 + §8.6 | re-INVITE with updated participant XML |
| Communication failure| RFC 7866 §11.1.1     | retry policy + soft-fail (recording is best-effort) |

## File layout

```
mod_siprec.c          module entry, app dispatch, config load
mod_siprec.h          public types (recording_t, recording_server_t, globals_t)

siprec_session.c      lifecycle: start/stop/pause/resume
siprec_session.h

siprec_sdp.c          SDP body builder (RFC 7866 §7)
siprec_sdp.h

siprec_metadata.c     RFC 7865 XML metadata builder
siprec_metadata.h

siprec_invite.c       SIP INVITE/BYE/re-INVITE via sofia-sip NUA
siprec_invite.h

siprec_media.c        switch_core_media_bug callback, RTP forwarding
siprec_media.h
```

## Phase plan

### Phase 1 — foundation (NO FS deps; pure C, unit-testable)

- [x] `siprec_sdp.c` — build the SDP half of the body. Inputs: array of
      tracks (label, codec, port, IP). Output: a `char *` with the
      full SDP. Includes `o=`, `s=`, `c=`, one `m=audio` per track,
      `a=label:N`, `a=sendonly` (RFC 7866 §7.4).
- [x] `siprec_metadata.c` — build the RFC 7865 metadata XML
      `<recording xmlns="urn:ietf:params:xml:ns:recording:1">` with
      `<datamode>complete</datamode>`, `<group>`, `<session>`,
      `<participant>` per leg, `<stream>` cross-references via
      `participant_session_assoc`. Schema lives in RFC 7865 §5.

### Phase 2 — SIP signaling

- [x] `siprec_invite.c` — issues the INVITE via
      `switch_ivr_originate` against the same sofia profile
      carrying the original call. The metadata XML and
      `Require: siprec` header are attached through the
      originate's `ovars` parameter (NOT the brace-prefix
      dial-string; the brace grammar terminates a value at
      the first `,`/`'`/`}` and would corrupt the multipart
      body). `process_mp` in sofia_media.c parses
      `<Content-Type>:~<extra-headers>\r\n<body>` and
      assembles `multipart/mixed` with the auto-generated SDP
      as part 1, our metadata as part 2.
- [x] BYE: `siprec_invite_send_bye` looks up the recording
      leg by stashed UUID via `switch_core_session_locate`
      and hangs up via `switch_channel_hangup(NORMAL_CLEARING)`.
      Sofia emits the BYE; idempotent — locate returns NULL
      when the dialog is already gone.
- [x] re-INVITE: `siprec_invite_reinvite` pushes the updated
      metadata as a fresh `sip_multipart` entry and drives
      the re-INVITE via `SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT`.
      mod_sofia's handler calls `switch_core_media_set_local_sdp`
      + `sofia_glue_do_invite` to emit on the existing dialog.
      Locate-by-UUID is used so a torn-down recording leg
      doesn't UAF the message dispatch.

### Phase 3 — media tap & RTP fork

- [x] `siprec_media.c` — `switch_core_media_bug_add()` with the
      observe-only `SMBF_READ_STREAM | SMBF_WRITE_STREAM` flag set
      (the canonical pattern from `record_callback` in
      switch_ivr_async.c). Callback handles `SWITCH_ABC_TYPE_READ`
      / `_WRITE` and pulls each frame via
      `switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)`.
      One UDP socket per stream — opened ipv4-only at attach
      with kernel-assigned ephemeral source port. Per-stream
      endpoint comes from parsing `sip_remote_sdp_str` in
      `siprec_invite.c:parse_remote_sdp_streams`.
- [x] Codec passthrough: `read_codec->ianacode` selects PCMU/PCMA
      at attach time. v1 assumes 8 kHz mono 20 ms ptime.
- [x] RTP framing: inline G.711 encoders (l16_to_ulaw /
      l16_to_alaw, INT16_MIN-safe via int promotion) +
      RFC 3550 §5.1 header packing. SSRC pulled from
      `/dev/urandom` per RFC 3550 §8.1; sequence + timestamp
      incremented in lock-step with each frame; M-bit set on
      the first packet after silence per RFC 3551 §4.1.
- [ ] DTMF tone forking (RFC 7866 §8.4) — passes through
      transparently via the bug's read path; explicit RFC 2833
      passthrough is a future enhancement.

### Phase 4 — lifecycle integration

- [x] `stop_recording_session` drives the ordered teardown:
      `siprec_media_detach` (removes bug + closes UDP sockets) →
      `siprec_invite_send_bye` (BYE on recording dialog) → mutex /
      pool free.
- [x] `start_recording_session` builds the metadata XML, dispatches
      the INVITE via `siprec_invite_send`, then attaches the media
      bug with `siprec_media_attach`.
- [x] State-handler bound: `switch_channel_add_state_handler(
      channel, &state_handlers)` runs at the end of
      `start_recording_session`, so caller-side hangup of the
      original call automatically tears down the recording.
- [x] `siprec_pause` / `siprec_resume` apps — wire dialplan
      entry points through `siprec_change_direction`, which
      reads the recording leg's `sip_local_sdp_str`, runs it
      through `siprec_sdp_flip_direction` (bumps
      `o=session-version` per RFC 4566 §5.2 and swaps
      `a=sendonly` ⇄ `a=inactive`), and dispatches the
      re-INVITE through `siprec_invite_reinvite`.

### Phase 5 — config

- [x] `autoload_conf/siprec.conf.xml` schema:
      ```xml
      <configuration name="siprec.conf">
        <recording-servers>
          <recording-server name="default">
            <param name="host" value="127.0.0.1"/>
            <param name="port" value="5070"/>
            <param name="profile" value="external"/>
            <param name="src-realm" value="src.example.com"/>
            <!-- optional auth -->
            <param name="username" value=""/>
            <param name="password" value=""/>
          </recording-server>
        </recording-servers>
      </configuration>
      ```

### Phase 6 — testing

- [x] Unit tests for `siprec_sdp.c` — assertions covering
      RFC 7866 §7 line shape, RFC 4566 SDP shape,
      `a=group:DUP` correctly absent, mono vs stereo rtpmap,
      direction-flip helper round-trip, validation reject paths.
- [x] Unit tests for `siprec_metadata.c` — assertions
      covering RFC 7865 Appendix A schema element ordering,
      attribute presence, schema-strict participant /
      session / group / stream / assoc shapes, XML escaping
      of caller-supplied content (no entity injection).
      Run with `make -f Makefile.test test`; lint with
      `make -f Makefile.test lint` (cppcheck
      `--enable=all --check-level=exhaustive` clean).
- [x] `tests/README.md` — operator-facing first-deploy
      verification checklist with one row per code-path
      (multipart insertion, per-stream endpoint parsing,
      pause/resume, marker bit, SSRC randomness).
- [ ] Live integration against `cb-srs` — run
      `siprec-start-stop.xml` from callBroadcast's TwiML suite
      with mod_siprec built from this fork; the suite already
      checks for `EXECUTE.*siprec\(` in the journal and
      `[twiml] <uuid>: done`. Move from FAIL to PASS once the
      verification checklist passes on a live FreeSWITCH.

## Known gaps

- **Initial-offer SDP override**: a multi-track offer (two
  `m=audio` blocks, one per recorded direction, each with its
  own `a=label:N` per RFC 7866 §8.5) needs the SRC to control
  the SDP body carried on the initial INVITE. mod_sofia
  auto-generates that body single-track and there's no
  per-call hook today that lets mod_siprec inject a
  pre-built SDP. Until this lands, the SRC sends a
  single-track offer and records only the READ direction
  (RFC 7866 §7 explicitly permits this — "MAY send multiple
  streams" — but bidirectional recording is the obvious next
  step). SRTP via SDES (RFC 4568) is also gated on the same
  override — keymat must travel in the initial offer, which
  we don't control today.

## Non-goals (deferred)

- **Server-side (SRS)**: this module is SRC-only. The cb-srs
  receiver is implemented separately in Go.
- **Video tracks**: RFC 7866 supports them; v1 is audio-only.
- **IPv6 RTP fork**: the UDP fork in `siprec_media.c` is
  AF_INET only. An IPv6 negotiated endpoint is detected at
  attach and the recording is refused with a clear error.
- **Persistence/resume across module reloads**: a recording
  session is a per-call construct; module reload terminates
  active recordings (the shutdown handler detaches the bug
  and BYEs the recording leg before freeing pools). No
  state recovery.
