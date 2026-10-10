#!/usr/bin/env python3
"""Run production server-command entry/stop code with controlled ENet boundaries.

The send is paused after admission while another thread tears down the stream.
--baseline reads HEAD. Platform joins/sockets are fakes; no real network is used.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATH = 'src/ControlStream.c'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + PATH], cwd=ROOT, text=True)
          if '--baseline' in sys.argv else (ROOT / PATH).read_text())
def block(marker):
    start = source.index(marker)
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end + 1]

gate = ''
if '// Server-command lifetime gate.' in source:
    gate = source[source.index('// Server-command lifetime gate.'):source.index('// Process-lifetime lock: HDR queries')]
startup = block('int startControlStream(')
tail = startup[startup.rfind('\n    openServerCommands();'):] if '\n    openServerCommands();' in startup else '\n    return 0;\n}'
code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
#define LC_ASSERT assert
#define IS_SUNSHINE() sunshine
static bool sunshine=true,stopping=false,ConnectionInterrupted=true;
static int ctlSock=-1;
constexpr int INVALID_SOCKET=-1,CONTROL_STREAM_LINGER_TIMEOUT_SEC=1;
constexpr int IDX_EXEC_SERVER_CMD=0,CTRL_CHANNEL_SERVERCTL=0,ENET_PACKET_FLAG_RELIABLE=1;
static short table[]={1};static short* packetTypes=table;
static int object;static void* peer=nullptr;static void* client=nullptr;
static int referenceFrameControlQueue,frameFecStatusQueue,asyncCallbackQueue,idrFrameRequiredEvent;
static int lossStatsThread,requestIdrFrameThread,controlReceiveThread,asyncCallbackThread,invalidateRefFramesThread;
static std::atomic<bool> retired{false},gateWaiting{false};
static std::mutex latchMutex;static std::condition_variable latch;
static bool pauseSend=false,sendEntered=false,releaseSend=false;static int sends=0,retiredAccess=0;
static void PltSleepMs(int ms){if(ms){gateWaiting=true;std::this_thread::sleep_for(1ms);}else std::this_thread::yield();}
static void LbqSignalQueueShutdown(int*){}static void LbqSignalQueueDrain(int*){}
static void PltSetEvent(int*){}static void PltInterruptThread(int*){}static void PltJoinThread(int*){}
static void shutdownTcpSocket(int){}static void closeSocket(int){}
static bool isReferenceFrameInvalidationEnabled(){return false;}
static void gracefullyDisconnectEnetPeer(void*,void*,int){retired=true;}
static void enet_host_destroy(void*){retired=true;}
static bool sendMessageAndForget(short,short,const void*,uint8_t,uint32_t,bool){
 std::unique_lock<std::mutex> lock(latchMutex);++sends;
 if(pauseSend){sendEntered=true;latch.notify_all();latch.wait(lock,[]{return releaseSend;});}
 retiredAccess+=retired.load();return true;
}
GATE
static int publishSuccessfulStart(){START_TAIL
METHODS
int main(){
 int checks=0,failures=0;
 auto check=[&](bool ok,const char* text){++checks;printf("%s %s\n",ok?"PASS":"FAIL",text);failures+=!ok;};
 auto reset=[&]{peer=client=&object;stopping=false;retired=false;gateWaiting=false;sendEntered=false;releaseSend=false;pauseSend=false;sends=retiredAccess=0;};
 auto command=[](int kind){return kind?LiSendEmptyPayload():LiSendExecServerCmd(7);};
 reset();
 check(command(0)==-1&&command(1)==-1&&sends==0,"partially established control stream does not admit external commands");
 for(int kind=0;kind<2;kind++){
  reset();publishSuccessfulStart();pauseSend=true;int result=0;
  std::thread sender([&]{result=command(kind);});
  {std::unique_lock<std::mutex> lock(latchMutex);if(!latch.wait_for(lock,2s,[]{return sendEntered;}))std::abort();}
  std::thread stopper([]{stopControlStream();});
  auto deadline=std::chrono::steady_clock::now()+2s;
  while(!retired&&!gateWaiting&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  check(gateWaiting&&!retired,kind?"stop waits for an admitted empty-payload send":"stop waits for an admitted server command");
  check(command(kind)==-1,"closing rejects new commands while draining the admitted sender");
  {std::lock_guard<std::mutex> lock(latchMutex);releaseSend=true;latch.notify_all();}
  sender.join();stopper.join();
  check(result==1&&retiredAccess==0&&retired&&peer==nullptr&&client==nullptr,
        "admitted send finishes before peer/client retirement");
  check(command(kind)==-1,"command after teardown is rejected without touching retired resources");
 }
 reset();publishSuccessfulStart();
 check(command(0)==1&&command(1)==1&&sends==2,"a new successful session reopens command admission");
 sunshine=false;check(command(0)==-1&&command(1)==-1,"non-Sunshine sessions still reject custom commands");
 sunshine=true;stopControlStream();
 printf("%d checks, %d failures\n",checks,failures);return failures!=0;
}
'''.replace('GATE', gate).replace('START_TAIL', tail).replace('METHODS', '\n'.join(block(x) for x in ['int stopControlStream(', 'int LiSendExecServerCmd(', 'int LiSendEmptyPayload(']))
with tempfile.TemporaryDirectory(prefix='server-commands-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')], env=os.environ, timeout=15).returncode)
