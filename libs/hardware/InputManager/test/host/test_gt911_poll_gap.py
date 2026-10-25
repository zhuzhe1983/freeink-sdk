"""Exercise the production GT911 polling and recovery with a latched I2C frame."""

from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/InputManager.cpp").read_text()


def method(name):
    found = re.search(rf"(?:bool|void) InputManager::{name}\([^\n]*\) \{{.*?^\}}", source, re.M | re.S)
    assert found, name
    return found[0]


harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
namespace BoardConfig {
struct Touch {
  bool gt911CoordsAtByte0=true, swapXY=false, flipX=false, flipY=false;
  uint16_t rawMinX=0, rawMinY=0, rawMaxX=959, rawMaxY=539;
};
struct { Touch touch; } ACTIVE;
}
int absInt(int value) { return std::abs(value); }
struct InputManager {
  enum class Gt911SyncState : uint8_t { Ready, ClearPending, AwaitingRelease };
  Gt911SyncState gt911SyncState=Gt911SyncState::Ready;
  uint32_t gt911LastPollAt=0;
  static constexpr uint32_t GT911_STALE_POLL_MS=128;
  static constexpr unsigned MAX_TOUCH_CONTACTS=5;
  static constexpr unsigned HOME_KEY_LONG_PRESS_MS=700;
  static constexpr int TOUCH_TAP_SLOP_PX=28, TOUCH_TAP_RELEASE_SLOP_PX=60;
  struct TouchPoint { bool valid=false; uint16_t x=0,y=0; unsigned long timestamp=0; };
  struct Contact { uint8_t id=0; TouchPoint point; };
  struct Snapshot {
    uint8_t count=0,reportedCount=0; bool idsStable=false;
    Contact points[MAX_TOUCH_CONTACTS];
  } touchSnapshot;
  uint8_t gt911Addr=0x5d;
  TouchPoint touchPoint,touchDownPoint,touchUpPoint;
  bool touchPressed=false,touchPressedEvent=false,touchReleasedEvent=false;
  bool touchHomeKeyDown=false,touchHomeKeyEvent=false,touchHomeKeyTapEvent=false;
  bool touchHomeKeyLongEvent=false,touchHomeKeyLongFired=false;
  bool touchLongPressEvent=false,touchSuppressed=false;
  bool touchMovedBeyondTapSlop=false,touchMovedBeyondTapReleaseSlop=false;
  unsigned long touchHomeKeyDownAt=0,lastTouchHeldDurationMs=0;
  unsigned cancels=0,frames=0,acks=0,reads=0;
  uint8_t status=0;
  bool readOk=true,ackOk=true;
  uint16_t rawX=80,rawY=120;
  bool gt911ReadReg(uint16_t reg,uint8_t* buf,uint8_t len) {
    ++reads;
    if (!readOk) return false;
    if (reg==0x814e) { assert(len==1); *buf=status; return true; }
    assert(reg==0x8150); std::memset(buf,0,len);
    buf[0]=rawX; buf[1]=rawX>>8; buf[2]=rawY; buf[3]=rawY>>8;
    return true;
  }
  bool gt911ClearStatus() {
    ++acks;
    if (!ackOk) return false;
    status=0;
    return true;
  }
  void suppressTouchContact() { ++cancels; touchSuppressed=true; }
  void updateMultiTouchGesture(const Snapshot&,unsigned long) { ++frames; }
  uint16_t mapTouchAxis(uint16_t v,uint16_t lo,uint16_t hi,uint16_t max) {
    return std::min<unsigned>(max, std::max<int>(0,v-lo)*max/(hi-lo));
  }
  bool resyncGt911AfterPollGap(uint32_t now);
  void pollGt911(unsigned long now);
  void tick(uint32_t now,uint8_t frame) {
    touchPressedEvent=touchReleasedEvent=touchLongPressEvent=false;
    if (!touchPressed) touchSuppressed=false;
    status=frame; pollGt911(now);
  }
  void noEvents() const {
    assert(!touchPressedEvent && !touchReleasedEvent && !touchLongPressEvent);
    assert(!touchHomeKeyEvent && !touchHomeKeyTapEvent && !touchHomeKeyLongEvent);
  }
};
""" + method("resyncGt911AfterPollGap") + "\n" + method("pollGt911") + r"""
int main() {
  InputManager in;
  // Ordinary taps, stationary holds and releases retain the existing API.
  in.tick(10,0x81); assert(in.touchPressedEvent && in.touchPressed);
  assert(in.touchPoint.x==80 && in.touchPoint.y==120);
  in.tick(20,0); assert(in.touchPressed && !in.touchReleasedEvent);
  in.tick(30,0x80); assert(in.touchReleasedEvent && !in.touchPressed);
  assert(in.lastTouchHeldDurationMs==20);
  in={}; in.tick(10,0x81); in.tick(138,0x81);
#if FREEINK_DEVICE_PAPERS3
  // A stale held sample is drained, not delivered into the newly rendered page.
  in.noEvents(); assert(!in.touchPressed && in.touchSnapshot.count==0);
  assert(in.gt911SyncState==InputManager::Gt911SyncState::AwaitingRelease);
  in.tick(148,0); in.noEvents();
  in.tick(158,0x81); in.noEvents();
  in.tick(168,0x80); in.noEvents();
  assert(in.gt911SyncState==InputManager::Gt911SyncState::Ready);
  in.rawX=300; in.tick(178,0x81); assert(in.touchPressedEvent && in.touchPoint.x==300);
  in.tick(188,0x80); assert(in.touchReleasedEvent && !in.touchSuppressed);

  // A complete tap buffered while the loop was blocked cannot arrive late.
  in={}; in.tick(1000,0x81); in.noEvents();
  in.tick(1010,0x80); in.noEvents();
  in.tick(1020,0x81); assert(in.touchPressedEvent);

  // Idle recovery must not require an extra sacrificial tap.
  in={}; in.tick(1000,0); in.noEvents();
  in.tick(1010,0x81); assert(in.touchPressedEvent);
  in={}; in.tick(1000,0x80); in.tick(1010,0x81); assert(in.touchPressedEvent);

  // Neither a read failure nor a failed status acknowledgement admits stale data.
  in={}; in.readOk=false; in.tick(1000,0x81); in.noEvents();
  in.readOk=true; in.ackOk=false; in.tick(1010,0x81); in.noEvents();
  assert(in.gt911SyncState==InputManager::Gt911SyncState::ClearPending);
  in.ackOk=true; in.tick(1020,0x81); in.noEvents();
  in.tick(1030,0x80); in.noEvents();
  in.tick(1040,0x81); assert(in.touchPressedEvent);

  // Repeated gaps must not forget an unfinished finger, even with no new frame.
  in={}; in.tick(10,0x81); in.tick(1000,0); in.noEvents();
  in.tick(2000,0); in.noEvents();
  assert(in.gt911SyncState==InputManager::Gt911SyncState::AwaitingRelease);
  in.tick(2010,0x80); in.noEvents();
  in.tick(2020,0x81); assert(in.touchPressedEvent);

  // Normal poll cadence across the 32-bit millis rollover is not a long gap.
  in={}; in.gt911LastPollAt=UINT32_MAX-10;
  in.tick(5,0x81); assert(in.touchPressedEvent && in.cancels==0);
  in.tick(133,0x80); in.noEvents(); assert(in.cancels==1);
#else
  // Other GT911 boards keep their current polling contract.
  assert(in.touchPressed && in.cancels==0);
#endif
}
"""

with tempfile.TemporaryDirectory(prefix="freeink-gt911-") as directory:
    path = Path(directory)
    cpp = path / "test.cpp"
    cpp.write_text(harness)
    for papers3 in (0, 1):
        binary = path / f"gt911-{papers3}"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", f"-DFREEINK_DEVICE_PAPERS3={papers3}",
                        str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("GT911: normal/late taps, held contacts, idle, I2C failures, rollover and other-board checks passed")
