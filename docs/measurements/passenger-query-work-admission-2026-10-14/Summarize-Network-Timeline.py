"""Separate socket progress, pending receive data and catch-up CPU evidence."""
import argparse
import csv
import json
import pathlib

def read(path):
    with path.open(encoding='utf-8', newline='') as stream:
        return [{key: value if key == 'event' else int(value) for key, value in row.items()} for row in csv.DictReader(stream)]

def analyze(root):
    peers = {role: read(root / role / 'network-timeline.csv') for role in ['server', 'client-initial', 'client-late']}
    findings = []
    for role in ['client-initial', 'client-late']:
        rows = peers[role]
        for timeout in [r for r in rows if r['event'] == 'receive_timeout']:
            identity = next(r for r in rows if r['event'] == 'socket_identity' and r['socket'] == timeout['socket'] and r['b'] == 17523)
            server_identity = next(r for r in peers['server'] if r['event'] == 'socket_identity' and r['a'] == 17523 and r['b'] == identity['a'])
            end = max((r for r in rows if r['event'] == 'catchup_end' and r['qpc_us'] <= timeout['qpc_us']), key=lambda r:r['qpc_us'])
            begin = max((r for r in rows if r['event'] == 'catchup_begin' and r['frame'] == end['d'] and r['target'] == end['target'] and r['qpc_us'] < end['qpc_us']), key=lambda r:r['qpc_us'])
            lower, upper = begin['qpc_us'], end['qpc_us']
            sent = [r for r in peers['server'] if r['event'] == 'tcp_totals' and r['socket'] == server_identity['socket'] and lower <= r['qpc_us'] <= upper]
            queued = [r for r in peers['server'] if r['event'] == 'server_frame_queued' and r['socket'] == server_identity['socket'] and lower <= r['qpc_us'] <= upper]
            blocks = [r for r in peers['server'] if r['event'] == 'tcp_block_counts' and r['socket'] == server_identity['socket'] and lower <= r['qpc_us'] <= upper]
            # Difference entirely inside the catch-up interval is a lower
            # bound, not an extrapolation of the 500ms samples at its edges.
            accepted_bytes = sent[-1]['a'] - sent[0]['a'] if len(sent) > 1 else 0
            receive_calls_inside = [r for r in rows if r['event'] == 'receive_enter' and lower < r['qpc_us'] < upper]
            thread_seconds, wall_seconds = end['b'] / 10000000, end['a'] / 1000000
            pending = None if timeout['b'] == 2**64 - 1 else timeout['b']
            evidence = {'role': role, 'local_port': identity['a'], 'server_socket': server_identity['socket'],
                'timeout_frame': timeout['frame'], 'timeout_qpc_us': timeout['qpc_us'], 'last_protocol_packet_age_ms': timeout['a'],
                'pending_socket_bytes_before_close': pending, 'catchup_initial_frame': begin['frame'], 'catchup_target_frame': begin['target'],
                'catchup_final_frame': end['frame'], 'catchup_frames': end['frame'] - begin['frame'],
                'catchup_begin_qpc_us': lower, 'catchup_end_qpc_us': upper, 'catchup_wall_seconds': wall_seconds,
                'catchup_game_thread_cpu_seconds': thread_seconds, 'catchup_process_cpu_seconds': end['c'] / 10000000,
                'catchup_game_thread_cpu_to_wall': thread_seconds / wall_seconds if wall_seconds else None,
                'server_os_accepted_bytes_lower_bound_during_catchup': accepted_bytes,
                'server_tx_samples_during_catchup': len(sent), 'server_tx_last_success_qpc_us': sent[-1]['c'] if sent else None,
                'server_queued_frames_range_during_catchup': [queued[0]['frame'], queued[-1]['frame']] if queued else None,
                'server_send_would_block_delta_during_catchup': blocks[-1]['a'] - blocks[0]['a'] if len(blocks) > 1 else None,
                'receive_entry_samples_inside_catchup': len(receive_calls_inside)}
            if pending and accepted_bytes > 0 and wall_seconds > 20 and not receive_calls_inside:
                evidence['classification'] = 'client_cpu_catchup_with_pending_data' if thread_seconds / wall_seconds >= .7 else 'client_catchup_stall_with_pending_data_cpu_not_proven'
            else:
                evidence['classification'] = 'insufficient_combined_evidence'
            findings.append(evidence)
    summaries = {}
    for role in ['client-initial', 'client-late']:
        rows = peers[role]
        ends = [row for row in rows if row['event'] == 'catchup_end']
        summaries[role] = {'observed_catchup_batches': len(ends),
            'maximum_catchup_wall_seconds': max((row['a'] / 1000000 for row in ends), default=0),
            'maximum_receive_entry_gap_seconds': max((row['a'] / 1000000 for row in rows if row['event'] == 'receive_entry_gap'), default=0),
            'maximum_batch_frames': max((row['frame'] - row['d'] for row in ends), default=0)}
    return {'local_run': str(root), 'findings': findings, 'peer_summary': summaries, 'clock': 'same-host QPC microseconds',
        'sampling': 'TCP and server-frame samples every 500ms; catch-up and timeout events immediate',
        'cpu_units': 'thread and process user+kernel 100ns; wall QPC microseconds',
        'timeout_changed': False, 'network_success_claimed': False,
        'limits': ['Positive send means OS accepted bytes; FIONREAD additionally proves bytes were waiting at this client.',
            'CPU ratio <70% does not prove CPU saturation; process CPU includes workers and may exceed wall time.',
            'This observer measures processing/IO timing, not a proof of route determinism.']}

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('run', type=pathlib.Path)
    p.add_argument('--archive', type=pathlib.Path, required=True)
    a = p.parse_args()
    result = analyze(a.run.resolve())
    a.archive.mkdir(parents=True, exist_ok=True)
    (a.archive / 'timeline-analysis.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
