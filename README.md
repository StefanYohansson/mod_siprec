# mod_siprec

A FreeSWITCH module that implements [RFC 7866][rfc7866] SIPREC
(Session Recording Protocol) as a Session Recording Client (SRC),
with [RFC 7865][rfc7865]-compliant metadata.

For each call you want to record, mod_siprec opens a parallel
SIP INVITE ([RFC 3261][rfc3261]) to a configured Session
Recording Server (SRS), attaches a `multipart/mixed`
([RFC 2046][rfc2046]) body containing both the [SDP][rfc4566]
offer and an XML metadata document, taps the original call's
audio via a FreeSWITCH media bug, and forks the captured audio
as [RFC 3550][rfc3550] RTP to the endpoints negotiated in the
SRS's 200 OK answer.

[rfc2046]: https://datatracker.ietf.org/doc/html/rfc2046
[rfc3261]: https://datatracker.ietf.org/doc/html/rfc3261
[rfc3550]: https://datatracker.ietf.org/doc/html/rfc3550
[rfc3551]: https://datatracker.ietf.org/doc/html/rfc3551
[rfc3711]: https://datatracker.ietf.org/doc/html/rfc3711
[rfc4566]: https://datatracker.ietf.org/doc/html/rfc4566
[rfc4568]: https://datatracker.ietf.org/doc/html/rfc4568
[rfc5888]: https://datatracker.ietf.org/doc/html/rfc5888
[rfc7865]: https://datatracker.ietf.org/doc/html/rfc7865
[rfc7866]: https://datatracker.ietf.org/doc/html/rfc7866

## Status

All paths build, lint clean, and the unit-test suite for the
SDP / metadata builders passes 93/93 assertions. The dispatch /
media / signalling pipeline has been audited and the broken
pieces from the original fork have been replaced. Live
integration against `cb-srs` is documented in
[`tests/README.md`](tests/README.md) as the operator
verification path; production interop is verified against
[TransNexus ClearIP][clearip] as well.

[clearip]: https://transnexus.com/clearip/

| Concept | Spec | Status |
|---|---|---|
| SRC INVITE with `Require: siprec` | [RFC 7866 §6.1][rfc7866-6.1] | ✅ via `switch_ivr_originate` ovars |
| `+sip.src` Contact feature tag | [RFC 7866 §5.2.1][rfc7866-5.2.1] | ✅ via `sip_invite_contact_params=~+sip.src` ovar; the leading `~` tells `sofia_overcome_sip_uri_weakness` (sofia_glue.c:854,891) to place the tag AFTER the closing `>`, yielding `Contact: <sip:src@host:port>;+sip.src` per the spec |
| `multipart/mixed` (SDP + metadata) | [RFC 7866 §6.1.2][rfc7866-6.1.2] / [RFC 2046][rfc2046] | ✅ `sip_multipart` channel var |
| BYE on hangup | [RFC 7866 §6.4][rfc7866-6.4] | ✅ on_destroy state-handler |
| pause / resume re-INVITE | [RFC 7866 §6.4][rfc7866-6.4] | ✅ `siprec_pause` / `siprec_resume` apps; SDP direction-flip preserves negotiated session |
| sendonly direction on SRC streams | [RFC 7866 §7.4][rfc7866-7.4] | ✅ sofia auto-gen offer |
| `a=label:N` per stream | [RFC 7866 §8.5][rfc7866-8.5] | ✅ per-block sequential labels (1st m= → `label:1`, 2nd → `label:2`, …) injected via post-originate re-INVITE on the auto-generated SDP. mod_sofia's auto-gen is single-track today, so the wire effect is `label:1`; the moment a multi-track offer path lands (sofia SDP-override hook OR `siprec_sdp_build`-driven originate) the same call site picks up `label:1` + `label:2` + … without code changes |
| Single-direction SRC (READ only) | [RFC 7866 §7][rfc7866-7] | ✅ explicit single-stream operation: one `m=audio` block in offer, one `<stream>` in metadata, RTP forked from the bug's READ direction (far-party voice on the bug-host leg). RFC 7866 §7 permits "MAY" send multiple streams; we currently send one. WRITE-direction frames are consumed from the bug's queue and dropped (siprec_media.c:240) so back-pressure on the original session is avoided. Bidirectional recording is the v1.4.0 follow-up and gates on the same multi-track offer path as the §8.5 row above |
| DTMF tone forking | [RFC 7866 §8.4][rfc7866-8.4] | ✅ passes through the audio bug |
| communication-failure soft-fail | [RFC 7866 §11.1.1][rfc7866-11.1.1] | ✅ original call unaffected on dispatch failure |
| SRS failover (multiple endpoints, ordered) | [RFC 7866 §11.1.1][rfc7866-11.1.1] | ✅ multiple `<recording-server>` entries, walked in config order |
| SRTP for the recording RTP fork | [RFC 7866 §11.2][rfc7866-11.2] / [RFC 3711][rfc3711] / [RFC 4568][rfc4568] | ❌ not supported. SDES keymat must travel in the initial offer (RFC 4568 §5.1) and our offer is sofia auto-gen which doesn't carry `a=crypto`. The clean path needs the same offer-time SDP-override hook the multi-track work needs. SRSes that require SRTP will reject our `RTP/AVP` offer with `488 Not Acceptable Here`; failover or pin a strict-SRTP-not-required SRS. |
| SIPS transport for SRC→SRS | [RFC 7866 §11.3][rfc7866-11.3] | ✅ `transport=tls` config |
| SDP body shape (`v=`, `o=`, `s=`, `c=`, `t=`, `m=`, `a=rtpmap`, `a=ptime`, `a=label`, `a=sendonly`) | [RFC 4566][rfc4566] / [RFC 7866 §7][rfc7866-7] | ✅ `siprec_sdp_build` (offered to operators that build their own; sofia auto-gen used otherwise) |
| Pause/resume SDP direction-flip with `o=` version bump | [RFC 4566 §5.2][rfc4566] | ✅ `siprec_sdp_flip_direction` |
| RTP packet framing (V=2, M-bit at talkspurt start, big-endian seq/ts/SSRC) | [RFC 3550 §5.1][rfc3550] / [RFC 3551 §4.1][rfc3551] | ✅ `siprec_media.c` |
| Random SSRC | [RFC 3550 §8.1][rfc3550] | ✅ /dev/urandom seed |
| G.711 µ-law / A-law encoders | [G.711][g711] | ✅ inline encoders, INT16_MIN-safe |
| `<recording>` schema (top-level element + sequence) | [RFC 7865 §5 / Appendix A][rfc7865] | ✅ |
| `<datamode>` (`complete` + `partial`) | [RFC 7865 §5.1][rfc7865] | ✅ |
| `<group>` (`group_id`, `<associate-time>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<session>` (`session_id`, `<reason>`, `<group-ref>`, `<start-time>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<participant>` (`participant_id`, `<nameID>`) | [RFC 7865 Appendix A][rfc7865] | ✅ schema-strict (no inline send/recv, no session_id attr) |
| `<stream>` (`stream_id`, `session_id`, `<label>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<participantsessionassoc>` / `<participantstreamassoc>` | [RFC 7865 Appendix A][rfc7865] | ✅ |
| XML escaping for caller-supplied content | [RFC 7865 §5][rfc7865] | ✅ &amp; &lt; &gt; &quot; &apos; |

[rfc7866-5.2.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-5.2.1
[rfc7866-6.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1
[rfc7866-6.1.2]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1.2
[rfc7866-6.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.4
[rfc7866-7]: https://datatracker.ietf.org/doc/html/rfc7866#section-7
[rfc7866-7.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-7.4
[rfc7866-8.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.4
[rfc7866-8.5]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.5
[rfc7866-11.1.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.1.1
[rfc7866-11.2]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.2
[rfc7866-11.3]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.3
[g711]: https://www.itu.int/rec/T-REC-G.711

## Build

mod_siprec uses FreeSWITCH's in-tree autotools build. Stage the
sources alongside your FS source tree, register the module in
`build/modules.conf.in`, and bootstrap:

```sh
ln -sf $PWD src/mod/applications/mod_siprec
echo 'applications/mod_siprec' >> build/modules.conf.in
./bootstrap.sh && ./configure
make mod_siprec-install
```

Drop the config into FS's `autoload_configs`:

```sh
cp autoload_conf/siprec.conf.xml /usr/local/freeswitch/conf/autoload_configs/
```

Reload:

```sh
fs_cli -x 'reload mod_siprec'
```

### Standalone unit tests + lint (no FS required)

The SDP and metadata builders are pure C and exercised by a
standalone test target:

```sh
make -f Makefile.test test    # 77 / 77 assertions
make -f Makefile.test lint    # cppcheck --enable=all clean
```

## Configuration

`autoload_conf/siprec.conf.xml`:

```xml
<configuration name="siprec.conf"
               description="SIPREC (RFC 7866) module config">
  <settings>
    <param name="src-enabled" value="true"/>
    <param name="srs-enabled" value="false"/>
  </settings>
  <recording-servers>
    <recording-server name="default">
      <settings>
        <param name="host" value="127.0.0.1"/>
        <param name="port" value="5070"/>
        <param name="register" value="false"/>
        <param name="username" value=""/>
        <param name="password" value=""/>
      </settings>
    </recording-server>
  </recording-servers>
</configuration>
```

Multiple `<recording-server>` entries can coexist; the dialplan
chooses one by name.

## Dialplan usage

```xml
<extension name="record-with-siprec">
  <condition>
    <action application="siprec" data="default"/>
    <!-- ...the rest of your call flow... -->
  </condition>
</extension>
```

Pause and resume:

```xml
<action application="siprec_pause"  data="default"/>
<!-- audio bug stays attached, SRS records silence (a=inactive) -->
<action application="siprec_resume" data="default"/>
```

Hang-up tears the recording dialog down automatically — the
module installs an `on_destroy` state-handler when the recording
starts.

## Architecture

```
                                 ┌─────────────────────┐
                                 │   FreeSWITCH        │
                                 │                     │
   carrier ──────────RTP─────────┤  original call leg  │
                                 │      │              │
                                 │      ▼              │
                                 │  ┌────────────┐     │
                                 │  │ media bug  │     │
                                 │  │ (siprec)   │     │
                                 │  └────────────┘     │
                                 │      │              │
                                 │      ▼              │
                                 │  PCMU/PCMA encode   │
                                 │  RFC 3550 RTP pack  │
                                 │      │              │
                                 │      ▼              │
                                 │  recording leg ─────┼─INVITE─┐
                                 │  (sofia originate)  │ +RTP   │
                                 └─────────────────────┘        │
                                                                ▼
                                                         ┌─────────────┐
                                                         │ SRS         │
                                                         │ (cb-srs,    │
                                                         │  ClearIP,   │
                                                         │  Genesys,   │
                                                         │  NICE, ...) │
                                                         └─────────────┘
```

Files:

- **`mod_siprec.{c,h}`** — module entry, app dispatch, config load.
- **`recording_session.{c,h}`** — start / stop session lifecycle,
  state-handler binding.
- **`siprec_invite.{c,h}`** — SIP INVITE / BYE / re-INVITE via
  FreeSWITCH's sofia profile.
- **`siprec_media.{c,h}`** — media bug callback + RFC 3550 RTP
  fork, inline G.711 µ-law / A-law encoders.
- **`siprec_sdp.{c,h}`** — [RFC 7866 §7][rfc7866-7] SDP body
  builder + `siprec_sdp_flip_direction` helper used by the
  pause/resume re-INVITE path.
- **`siprec_metadata.{c,h}`** — [RFC 7865 Appendix A][rfc7865]
  schema-conformant metadata XML builder with full XML-entity
  escaping.
- **`siprec_test.c`** — unit-test assertions for the builders
  and the SDP direction-flip helper.
- **`autoload_conf/siprec.conf.xml`** — module config schema.
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — phase plan + RFC mapping.
- **[`tests/README.md`](tests/README.md)** — operator field-test
  checklist for first deploy.

## Contributing

Issues and PRs welcome. Two things to keep in mind:

1. **The SDP and metadata builders are pure C.** Any change to
   `siprec_sdp.c` / `siprec_metadata.c` must keep `make -f
   Makefile.test test` and `make -f Makefile.test lint` green.
2. **The FS-dependent files** can't be unit-tested without a
   FreeSWITCH source tree. Use the `tests/README.md` field-test
   checklist on a live build before merging behavior changes
   in `siprec_invite.c` / `siprec_media.c` / `recording_session.c`.

## Why this fork

This module started as a fork of
[StefanYohansson/mod_siprec][upstream]. The upstream readme
describes itself as an "initial idea, not working yet" and the
code reflects that: dispatching the `siprec` app crashed
FreeSWITCH within a few hundred milliseconds because of an
uninitialised memory pool, a NULL key into APR's hash, a
duplicate-detect lookup against the wrong hash, and a double-
free of an XML child after its parent had already been released.

This fork:

- fixes those four crash bugs on the dispatch path,
- fixes two memory-pool leaks on the start/stop paths,
- replaces the empty stub of `start_recording_session` with a
  full SRC pipeline (multipart MIME, SDP, metadata XML, INVITE,
  media bug, RTP fork, BYE),
- ships a unit test suite that runs in ~0.2 s with no FreeSWITCH
  dependency, plus a `cppcheck --enable=all
  --check-level=exhaustive` clean codebase.

The full feature set (RFC 7865 metadata, RFC 7866 §6.4 pause /
resume, RFC 7866 §5.2.1 `+sip.src` Contact tag, RFC 7866 §8.5
`a=label:N`, etc.) is documented in the **Status** table near
the top of this file.

[upstream]: https://github.com/StefanYohansson/mod_siprec

## 🙌 Contributors

We welcome contributions! Thanks to these awesome people:

- [Michael Mavroudis](https://github.com/mavroudis) - Lead Developer & Architect

## 💖 Sponsors

Proudly supported by:

| Sponsor | Contribution |
|---------|--------------|
| [VoiceTel Communications](http://www.voicetel.com) | Primary development and testing infrastructure |

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Files derived from the original
[StefanYohansson/mod_siprec][upstream] (which is itself derived
from FreeSWITCH) retain their MPL 1.1 license headers and are
dual-licensed under MPL 1.1 / MIT. New files written for this
fork (`siprec_sdp.*`, `siprec_metadata.*`, `siprec_invite.*`,
`siprec_media.*`, `siprec_test.c`) are MIT only.
