"""Read-only connection close diagnostics in an isolated translation unit."""
import argparse
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
build = a.build.resolve()
source = root / 'src/network/network_client.cpp'
text = source.read_text(encoding='utf-8')

def include(match):
    resolved = (source.parent / match[1]).resolve()
    return '#include "' + resolved.as_posix() + '"' if resolved.exists() else match[0]

text = re.sub(r'#include "([^"]+)"', include, text)
for signature, message in [
    ('NetworkRecvStatus ClientNetworkGameSocketHandler::CloseConnection(NetworkRecvStatus status)',
     'Debug(net, 0, "Final close probe: status={} frame={} game_mode={}", int(status), _frame_counter, int(_game_mode));'),
    ('void ClientNetworkGameSocketHandler::ClientError(NetworkRecvStatus res)',
     'Debug(net, 0, "Final error probe: status={} frame={}", int(res), _frame_counter);'),
]:
    marker = signature + '\n{'
    assert text.count(marker) == 1
    text = text.replace(marker, marker + '\n\t' + message)
marker = '\tif (lag > std::chrono::seconds(20)) {'
assert text.count(marker) == 1
text = text.replace(marker, marker + '\n\t\tDebug(net, 0, "Final receive timeout probe: idle_ms={} frame={} server_frame={}", std::chrono::duration_cast<std::chrono::milliseconds>(lag).count(), _frame_counter, _frame_counter_server);')
target = build / 'network-probe/network_client.cpp'
target.write_text(text, encoding='utf-8')
project = build / 'openttd_lib.vcxproj'
xml = project.read_text(encoding='utf-8-sig')
assert xml.count(str(source)) == 1
project.write_text(xml.replace(str(source), str(target)), encoding='utf-8-sig')
