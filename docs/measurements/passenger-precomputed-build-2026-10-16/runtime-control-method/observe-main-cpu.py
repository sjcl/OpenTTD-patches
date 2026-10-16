import ctypes,json,pathlib,time
from ctypes import wintypes
root=pathlib.Path.cwd()/'build-msvc'
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenThread.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];k.OpenThread.restype=wintypes.HANDLE
k.GetThreadTimes.argtypes=[wintypes.HANDLE,*([ctypes.POINTER(wintypes.FILETIME)]*4)]
k.QueryThreadCycleTime.argtypes=[wintypes.HANDLE,ctypes.POINTER(ctypes.c_ulonglong)]
k.CloseHandle.argtypes=[wintypes.HANDLE]
def sample(h):
 ts=[wintypes.FILETIME() for _ in range(4)];cy=ctypes.c_ulonglong()
 if not k.GetThreadTimes(h,*[ctypes.byref(x) for x in ts]):raise ctypes.WinError(ctypes.get_last_error())
 if not k.QueryThreadCycleTime(h,ctypes.byref(cy)):raise ctypes.WinError(ctypes.get_last_error())
 return dict(wall=time.perf_counter(),cpu_seconds=sum((x.dwHighDateTime<<32)|x.dwLowDateTime for x in ts[2:])*1e-7,cycles=cy.value)
for label in ['old','new']:
 d=root/('runtime-main-core-'+label);audit=d/'affinity-audit.json'
 deadline=time.monotonic()+1800
 while not audit.exists():
  if time.monotonic()>deadline:raise TimeoutError('affinity audit missing '+label)
  time.sleep(.2)
 a=json.loads(audit.read_text(encoding='utf-8-sig'));h=k.OpenThread(64,False,a['thread_id'])
 if not h:raise ctypes.WinError(ctypes.get_last_error())
 try:
  while True:
   if time.monotonic()>deadline:raise TimeoutError('start missing '+label)
   log=d/'on-1-console.txt'
   if log.exists() and 'Console log file closed.' in log.read_text(encoding='utf-8',errors='replace'):break
   time.sleep(.05)
  start=sample(h);already_finished=(d/'actual-ticks.json').exists()
  while not (d/'actual-ticks.json').exists():
   if time.monotonic()>deadline:raise TimeoutError('finish missing '+label)
   time.sleep(.05)
  end=sample(h)
  r=dict(pid=a['pid'],thread_id=a['thread_id'],start=start,end=end,already_finished_at_start=already_finished,
   elapsed_seconds=end['wall']-start['wall'],main_cpu_seconds=end['cpu_seconds']-start['cpu_seconds'],main_cycles=end['cycles']-start['cycles'],
   method='External observation of game_start log closure and exact-ticks output; 50ms polling; auxiliary interval includes bounded boundary observation error.')
  (d/'thread-cpu-window.json').write_text(json.dumps(r,indent=2)+'\n');print(label,json.dumps(r),flush=True)
 finally:k.CloseHandle(h)
