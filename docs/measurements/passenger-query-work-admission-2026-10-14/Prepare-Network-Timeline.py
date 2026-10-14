"""Observe TCP progress and catch-up CPU in copied TUs; never change timeouts."""
import argparse
import hashlib
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[3]

def once(text, old, new):
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
a = p.parse_args()
build = a.build.resolve()
folder = build / 'network-timeline'
folder.mkdir(exist_ok=True)
header = folder / 'timeline.h'
header.write_text(r'''#pragma once
#include <windows.h>
#include <fstream>
#include <map>
#include <mutex>
namespace NetworkTimeline {
inline uint64_t Now() {
    static const uint64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); uint64_t n = uint64_t(c.QuadPart);
    return (n / frequency) * 1000000 + ((n % frequency) * 1000000) / frequency;
}
inline uint64_t Time(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
inline uint64_t Cpu(bool process) {
    FILETIME a, b, kernel, user;
    bool okay = process ? GetProcessTimes(GetCurrentProcess(), &a, &b, &kernel, &user) : GetThreadTimes(GetCurrentThread(), &a, &b, &kernel, &user);
    return okay ? Time(kernel) + Time(user) : 0;
}
inline std::mutex mutex;
inline void Row(const char *event, uint64_t socket, uint32_t frame, uint32_t target, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0) {
    std::lock_guard lock(mutex);
    static std::ofstream out("network-timeline.csv");
    static bool first = true;
    if (first) { out << "qpc_us,pid,tid,event,socket,frame,target,a,b,c,d\n"; first = false; }
    out << Now() << ',' << GetCurrentProcessId() << ',' << GetCurrentThreadId() << ',' << event << ',' << socket << ',' << frame << ',' << target << ',' << a << ',' << b << ',' << c << ',' << d << '\n';
    out.flush();
}
struct Counter { uint64_t tx=0, rx=0, tx_at=0, rx_at=0, report=0, tx_block=0, rx_block=0; };
inline std::map<uint64_t, Counter> counters;
inline void SocketIdentity(SOCKET socket) {
    if (counters.contains(uint64_t(socket))) return;
    counters[uint64_t(socket)] = {};
    sockaddr_in local{}, remote{}; int n=sizeof(local), m=sizeof(remote);
    int a = getsockname(socket, reinterpret_cast<sockaddr *>(&local), &n);
    int b = getpeername(socket, reinterpret_cast<sockaddr *>(&remote), &m);
    Row("socket_identity", uint64_t(socket), 0, 0, a == 0 ? ntohs(local.sin_port) : 0, b == 0 ? ntohs(remote.sin_port) : 0);
}
inline void Io(SOCKET socket, bool send, int64_t result, bool would_block=false) {
    SocketIdentity(socket);
    auto &v = counters[uint64_t(socket)]; uint64_t now = Now();
    if (result > 0) { if (send) { v.tx += result; v.tx_at=now; } else { v.rx += result; v.rx_at=now; } }
    if (would_block) { if (send) ++v.tx_block; else ++v.rx_block; }
    if (now >= v.report + 500000 || (result <= 0 && !would_block)) {
        v.report=now;
        Row("tcp_totals", uint64_t(socket), 0, 0, v.tx, v.rx, v.tx_at, v.rx_at);
        Row("tcp_block_counts", uint64_t(socket), 0, 0, v.tx_block, v.rx_block);
        if (result <= 0 && !would_block) Row(send ? "tcp_send_closed_or_error" : "tcp_recv_closed_or_error", uint64_t(socket), 0, 0, uint64_t(result));
    }
}
inline void ReceiveEnter(SOCKET socket, uint32_t frame, uint32_t target) {
    static uint64_t previous=0, report=0, entries=0;
    uint64_t now=Now(), gap=previous ? now-previous : 0; previous=now; ++entries;
    if (gap >= 500000) Row("receive_entry_gap", uint64_t(socket), frame, target, gap);
    if (now >= report + 500000) { report=now; Row("receive_enter", uint64_t(socket), frame, target, entries); }
}
struct Catchup {
    uint64_t start, thread_cpu, process_cpu; uint32_t initial, target; const uint32_t *current;
    Catchup(const uint32_t &frame, uint32_t target) : start(Now()), thread_cpu(Cpu(false)), process_cpu(Cpu(true)), initial(frame), target(target), current(&frame) {
        Row("catchup_begin", 0, initial, target);
    }
    ~Catchup() { uint64_t end=Now(), t=Cpu(false), p=Cpu(true); Row("catchup_end", 0, *current, target, end-start, t-thread_cpu, p-process_cpu, initial); }
};
inline void FrameQueue(SOCKET socket, uint32_t client, uint32_t frame, uint32_t target) {
    static std::map<uint32_t,uint64_t> reports; uint64_t now=Now();
    if (now >= reports[client] + 500000) { reports[client]=now; Row("server_frame_queued", uint64_t(socket), frame, target, client); }
}
inline void Timeout(SOCKET socket, uint32_t frame, uint32_t target, uint64_t idle_ms) {
    u_long pending=0; int okay=ioctlsocket(socket, FIONREAD, &pending);
    Row("receive_timeout", uint64_t(socket), frame, target, idle_ms, okay == 0 ? pending : UINT64_MAX);
}
}
''', encoding='utf-8')
project = build / 'openttd_lib.vcxproj'
xml = project.read_text(encoding='utf-8-sig')
hashes = {}
for name in ['network/network.cpp', 'network/network_client.cpp', 'network/network_server.cpp', 'network/core/tcp.cpp']:
    source = ROOT / 'src' / name
    text = source.read_text(encoding='utf-8')
    text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source.parent / m[1]).resolve().as_posix() + '"'
                  if (source.parent / m[1]).resolve().exists() else m[0], text)
    marker = '#include "' + (ROOT / 'src/safeguards.h').as_posix() + '"'
    assert text.count(marker) == (2 if name == 'network/network.cpp' else 1)
    text = text.replace(marker, '#include "' + header.as_posix() + '"\n' + marker, 1)
    if name.endswith('/network.cpp'):
        marker = '\t\t\twhile (_frame_counter_server > _frame_counter) {'
        text = once(text, marker, '\t\t\tNetworkTimeline::Catchup observation(_frame_counter, _frame_counter_server);\n' + marker)
    elif name.endswith('/network_client.cpp'):
        marker = '/* static */ bool ClientNetworkGameSocketHandler::Receive()\n{'
        text = once(text, marker, marker + '\n\tNetworkTimeline::ReceiveEnter(my_client->sock, _frame_counter, _frame_counter_server);')
        marker = '\tif (lag > std::chrono::seconds(20)) {'
        text = once(text, marker, marker + '\n\t\tNetworkTimeline::Timeout(this->sock, _frame_counter, _frame_counter_server, std::chrono::duration_cast<std::chrono::milliseconds>(lag).count());\n\t\tDebug(net, 0, "Final timeline timeout: frame={} idle_ms={}", _frame_counter, std::chrono::duration_cast<std::chrono::milliseconds>(lag).count());')
    elif name.endswith('/network_server.cpp'):
        marker = 'NetworkRecvStatus ServerNetworkGameSocketHandler::SendFrame()\n{'
        text = once(text, marker, marker + '\n\tNetworkTimeline::FrameQueue(this->sock, uint32_t(this->client_id), _frame_counter, _frame_counter_max);')
    else:
        marker = '\t\tssize_t res = p.TransferOut(SocketSender{this->sock});'
        text = once(text, marker, marker + '\n\t\tif (res > 0) NetworkTimeline::Io(this->sock, true, res);')
        # Preserve error capture immediately after failing socket calls.
        start = text.index('SendPacketsState NetworkTCPSocketHandler::SendPackets(')
        end = text.index('std::unique_ptr<Packet> NetworkTCPSocketHandler::ReceivePacket()', start)
        tx, rx = text[start:end], text[end:]
        tx = once(tx, '\t\t\tNetworkError err = NetworkError::GetLast();', '\t\t\tNetworkError err = NetworkError::GetLast();\n\t\t\tNetworkTimeline::Io(this->sock, true, res, err.WouldBlock());')
        marker = 'res = p.TransferIn(SocketReceiver{this->sock});'
        assert rx.count(marker) == 2
        rx = rx.replace(marker, marker + '\n\t\tif (res > 0) NetworkTimeline::Io(this->sock, false, res);')
        marker = 'NetworkError err = NetworkError::GetLast();'
        assert rx.count(marker) == 2
        rx = rx.replace(marker, marker + '\n\t\t\tNetworkTimeline::Io(this->sock, false, res, err.WouldBlock());')
        text = text[:start] + tx + rx
    target = folder / pathlib.Path(name).name
    target.write_text(text, encoding='utf-8')
    xml = once(xml, str(source), str(target))
    hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
project.write_text(xml, encoding='utf-8-sig')
(folder / 'instrumentation.json').write_text(json.dumps({'copies': hashes, 'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'clock': 'QPC microseconds, same Windows host', 'tcp_sample_us': 500000, 'timeouts_changed': False,
    'cpu': 'GetThreadTimes / GetProcessTimes user+kernel in 100ns units', 'pending_bytes': 'FIONREAD, never drains socket'}, indent=2) + '\n', encoding='utf-8')
