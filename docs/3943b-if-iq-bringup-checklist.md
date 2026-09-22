# 3943B-S IF/IQ Bring-Up Checklist

## Do Not Repeat

Do not blindly run:

```text
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:REPort FREE
INITiate
```

This combination has produced zero PC bytes and can make the observed 3943B V2.1.0 reboot or drop the control socket.

Also do not repeat this live sequence by default:

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
TRACe:RECord:SELectivity REPort
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
```

It was accepted by SCPI with `SYSTem:ERRor? = 00` and still produced zero PC bytes, then the physical instrument was observed to reboot. Treat `SYSTem:IF:REMote:MODE SHORT` as reboot-risk on this firmware until Ceyear or the front-panel workflow confirms a safe prerequisite.

Do not use instrument-side IQ recording for the PC `.cap` workflow:

```text
TRACe:RECord:SELectivity RECord
TRACe:RECord:SOURce IQ
TRACe:RECord:STATe ON
```

Manual section 3.5.7 says IF data is output only when no IQ data recording operation is in progress. The PC `.cap` workflow must therefore use IQ/IF network reporting (`REPort`) and write the received bytes on the PC, not start the receiver's internal IQ record path.

## Read-Only Snapshot

Run first:

```powershell
.\build-ninja\ceyear_status_query.exe TCPIP::192.168.50.54::5025::SOCKET --probe-if-iq
```

Record:

- `*IDN?`
- `SYSTem:ERRor?`
- `FREQuency:MODE?`
- `MEASure:MODE?`
- `SYSTem:IF:REMote:MODE?`
- `TRACe:IQ:DATA:REPort?`
- `TRACe:PATH?`
- `TRACe:UDP?`
- `TRACe:TCP?`

## Front-Panel Checks

Confirm on the instrument before any stream start:

- The device is in a mode documented to output IF/IQ data.
- IF/IQ or digital IF streaming is enabled on the front panel if a local switch exists.
- Required options/licenses for digital IF/IQ output are present.
- The selected demodulation/measurement mode is compatible with IF/IQ output.
- The instrument remains responsive after enabling IF remote mode manually.

## First Write Test

Only after the above:

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
SYSTem:ERRor?
TRACe:UDP?
TRACe:PATH?
```

Expected:

- Error queue remains `00`.
- `TRACe:UDP?` shows the PC endpoint if query is supported.
- No IF stream is started yet.

## Stream Test Blocked

Do not run an IF stream test from software alone while the safe prerequisite for IF remote mode is unknown. The last controlled candidate was:

```text
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
```

It did not emit bytes and was followed by an observed device reboot. Do not add a trigger command or retry `SHORT` until the manual/front panel or vendor support confirms the missing device-side prerequisite.

The 2026-06-25 manual adds a likely missing IQ report enable command:

```text
TRIGger:IQ:REPort:STATe ON
```

The updated candidate for vendor-confirmed testing is a network report path, not an instrument record path:

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
TRACe:RECord:STATe OFF
TRACe:RECord:SELectivity REPort
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
TRIGger:IQ:REPort:STATe ON
```

This is still gated because it contains `SYSTem:IF:REMote:MODE SHORT`, which has been associated with observed reboots on this unit. Ask Ceyear to confirm command order and the safe stop sequence before live execution.

Stop immediately after the receive window or after any command error:

```text
TRIGger:IQ:REPort:STATe OFF
SYSTem:IF:REMote:MODE OFF
TRACe:RECord:SELectivity OFF
TRACe:RECord:STATe OFF
SYSTem:ERRor?
```

Escalate to `FREE` only after `MANual` emits non-zero bytes and the device remains stable.
