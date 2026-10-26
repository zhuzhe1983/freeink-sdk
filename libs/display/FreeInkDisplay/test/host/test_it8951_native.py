#!/usr/bin/env python3
"""Compile the actual IT8951 driver with a recording SPI controller, including failures."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
STUBS = {
'Arduino.h': r'''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstdlib>
#include <cassert>
namespace fake {
inline unsigned long now=0;
inline bool ready=true, failAlloc=false, failRefresh=false, lutBusy=false, failAfterPixels=false;
inline bool selected=false, awake=false, failAfterPreamble=false, failAfterReadDummy=false;
inline unsigned long readyAt=0;
inline unsigned command=0, reg=0;
inline std::vector<std::vector<uint8_t>> txs;
inline size_t allocations=0;
}
constexpr int HIGH=1, LOW=0, OUTPUT=1, INPUT=0;
inline unsigned long millis(){return fake::now;}
inline void delay(unsigned long n){fake::now+=n;}
inline int digitalRead(int){return fake::ready && fake::now>=fake::readyAt ? HIGH:LOW;}
inline void digitalWrite(int pin,int value){if(pin==15)fake::selected=value==LOW;}
inline void pinMode(int,int){}
''',
'SPI.h': r'''#pragma once
#include "Arduino.h"
constexpr int MSBFIRST=1,SPI_MODE0=0;
struct SPISettings { SPISettings(uint32_t,int,int){} };
struct SPIClass {
 std::vector<uint8_t> tx;
 void begin(int,int,int,int){}
 void beginTransaction(SPISettings){tx.clear();}
 uint16_t transfer16(uint16_t value){
  assert(fake::selected && fake::ready && fake::now>=fake::readyAt);
  tx.push_back(value>>8);tx.push_back(value&255);
  if(tx.size()==2){fake::readyAt=fake::now+3;if(fake::failAfterPreamble)fake::ready=false;}
  if(tx.size()==4 && tx[0]==0x10){fake::readyAt=fake::now+2;if(fake::failAfterReadDummy)fake::ready=false;}
  if(tx.size()<=4 || tx[0]!=0x10) return 0;
  if(fake::command==0x302){
   assert(fake::awake);
   constexpr uint16_t info[20]={540,960,0x36e0,0x12};
   return info[(tx.size()-6)/2];
  }
  return fake::reg==0x1224 && fake::lutBusy ? 1:0;
 }
 void writeBytes(uint8_t* bytes,size_t n){assert(fake::selected && fake::ready && fake::now>=fake::readyAt);tx.insert(tx.end(),bytes,bytes+n);}
 void endTransaction(){
  assert(!fake::selected);
  fake::txs.push_back(tx);
  if(tx.size()==4){
   const unsigned value=(unsigned(tx[2])<<8)|tx[3];
   if(tx[0]==0x60){fake::command=value;if(value==1)fake::awake=true;if(value==0x34&&fake::failRefresh)fake::lutBusy=true;}
   else if(tx[0]==0&&fake::command==0x10)fake::reg=value;
  }
  if(tx.size()>100 && tx[0]==0 && fake::failAfterPixels)fake::ready=false;
 }
};
inline SPIClass SPI;
''',
'BoardConfig.h': r'''#pragma once
#include <cstdint>
#define FREEINK_DRIVER_IT8951 1
namespace BoardConfig {
struct Pins {int8_t sclk=14,mosi=12,cs=15,busy=27,powerEnable=23;};
struct Sd {int8_t cs=4,sclk=14,mosi=12,miso=13;};
struct Profile {uint16_t displayWidth=960,displayHeight=540;Pins display;Sd sd;};
inline Profile ACTIVE;
}
''',
'esp_heap_caps.h': r'''#pragma once
#include "Arduino.h"
constexpr int MALLOC_CAP_SPIRAM=1;
inline void* heap_caps_malloc(size_t size,int){if(fake::failAlloc)return nullptr;auto*p=malloc(size);if(p)++fake::allocations;return p;}
inline void heap_caps_free(void*p){if(p)--fake::allocations;free(p);}
''',
'esp_log.h': '#pragma once\n#define ESP_LOGE(...) ((void)0)\n',
'esp_system.h': '#pragma once\nconstexpr int ESP_RST_DEEPSLEEP=5;\ninline int esp_reset_reason(){return 0;}\n',
'driver/gpio.h': '#pragma once\nusing gpio_num_t=int;\ninline void gpio_hold_dis(int){}\n',
'src/bus/EpdBus.h': '#pragma once\nnamespace freeink {enum class BusyPolarity{ActiveHigh,ActiveLow};class EpdBus{};}\n',
}
HARNESS = r'''
#include "src/driver/It8951Driver.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace freeink;
unsigned word(const std::vector<uint8_t>&tx){return (unsigned(tx[2])<<8)|tx[3];}
std::vector<unsigned> modes(){
 std::vector<unsigned> out;
 for(size_t i=0;i+5<fake::txs.size();++i)
  if(fake::txs[i].size()==4&&fake::txs[i][0]==0x60&&word(fake::txs[i])==0x34)
   out.push_back(word(fake::txs[i+5]));
 return out;
}
bool command(unsigned value){
 for(auto& tx:fake::txs)if(tx.size()==4&&tx[0]==0x60&&word(tx)==value)return true;
 return false;
}
void clear(){fake::txs.clear();fake::failRefresh=false;fake::failAfterPixels=false;fake::lutBusy=false;fake::ready=true;fake::readyAt=fake::now;fake::failAfterPreamble=false;fake::failAfterReadDummy=false;}
int main(){
 EpdBus bus;
 {
  It8951Driver driver;
  assert(!driver.beginGrayscale16());
  driver.begin(bus);
  assert(command(1) && fake::awake && !fake::selected);
  const size_t resident=fake::allocations;
  assert(resident==3);
  std::vector<uint8_t> bw(64800,0xff),lsb(64800,0),msb(64800,0);
  for(int repeat=0;repeat<2;++repeat){
   clear();auto* frame=driver.beginGrayscale16();assert(frame&&fake::allocations==resident+1);
   assert(!driver.beginGrayscale16());
   // All 16 gray values, including the nibble-boundary ordering.
   for(int i=0;i<8;++i)frame[i]=(2*i)|((2*i+1)<<4);
   assert(driver.commitGrayscale16(bw.data()));
   assert(fake::allocations==resident&&driver.opticalState()==OpticalState::Gray);
   assert(modes()==std::vector<unsigned>{2});
   bool pixels=false;
   for(auto&tx:fake::txs)if(tx.size()==259202){
    pixels=true;for(int i=0;i<8;++i)assert(tx[2+i]==((2*i)<<4|(2*i+1)));
    assert(tx.back()==255);
   }
   assert(pixels&&!command(2)&&!command(3));
   assert(!driver.commitGrayscale16(bw.data()));
  }
  // Gray -> fast menu must clean actual gray pixels, then B/W can use DU.
  clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
  assert(modes()==std::vector<unsigned>{2}&&driver.opticalState()==OpticalState::Bw);
  clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
  assert(modes()==std::vector<unsigned>{1});
  driver.copyGrayscaleLsb(bus,lsb.data());driver.copyGrayscaleMsb(bus,msb.data());
  clear();driver.displayGray(bus,bw.data(),false,nullptr,false);
  assert(modes()==std::vector<unsigned>{6}&&driver.opticalState()==OpticalState::Gray);
  clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Full,false);
  assert(modes()==std::vector<unsigned>{2});
  // Consecutive AA reading pages retain DU/DU4 cadence; no full clean each page.
  for (int page=0;page<8;++page) {
   clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
   assert(modes()==std::vector<unsigned>{1});
   clear();driver.displayGray(bus,bw.data(),false,nullptr,false);
   assert(modes()==std::vector<unsigned>{6});
  }
  clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
  assert(modes()==std::vector<unsigned>{2}); // configured periodic clean
  clear();driver.displayGray(bus,bw.data(),false,nullptr,false);
  assert(modes()==std::vector<unsigned>{2}); // AA matches the cleaning page
  // Cancelling a loan sends no pixels, frees memory and leaves optical state alone.
  clear();assert(driver.beginGrayscale16());driver.cancelGrayscale16();driver.cancelGrayscale16();
  assert(fake::allocations==resident&&modes().empty()&&!command(0x21));
  fake::failAlloc=true;assert(!driver.beginGrayscale16());fake::failAlloc=false;
  assert(fake::allocations==resident);
  // HRDY timeout before transfer: no RAM writes, refresh or sleep.
  clear();auto* frame=driver.beginGrayscale16();assert(frame);fake::ready=false;
  assert(!driver.commitGrayscale16(bw.data()));
  assert(!command(0x21)&&!command(0x34)&&driver.opticalState()==OpticalState::Unknown);
  auto n=fake::txs.size();driver.deepSleep(bus);assert(n==fake::txs.size());
  assert(fake::allocations==resident);
  // Busy after a preamble/dummy must close CS and stop before payload or pixels.
  clear();assert(driver.beginGrayscale16());fake::failAfterPreamble=true;
  assert(!driver.commitGrayscale16(bw.data()));
  assert(!fake::selected&&!command(0x21)&&!command(0x34)&&fake::allocations==resident);
  clear();assert(driver.beginGrayscale16());fake::failAfterReadDummy=true;
  assert(!driver.commitGrayscale16(bw.data()));
  assert(!fake::selected&&!command(0x21)&&!command(0x34)&&fake::allocations==resident);
  // Readiness failure after upload must prevent LD_IMG_END and DPY_AREA.
  clear();assert(driver.beginGrayscale16());fake::failAfterPixels=true;
  assert(!driver.commitGrayscale16(bw.data()));assert(!command(0x22)&&!command(0x34));
  // Waveform timeout cannot publish success or issue a standby/sleep command.
  clear();assert(driver.beginGrayscale16());fake::failRefresh=true;
  assert(!driver.commitGrayscale16(bw.data()));assert(driver.opticalState()==OpticalState::Unknown);
  assert(!command(2)&&!command(3)&&fake::allocations==resident);
  n=fake::txs.size();driver.deepSleep(bus);assert(n==fake::txs.size());
  // A later healthy request recovers with a full clean, not a differential update.
  clear();driver.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
  assert(modes()==std::vector<unsigned>{2}&&driver.opticalState()==OpticalState::Bw);
  clear();driver.deepSleep(bus);assert(command(3));
 }
 assert(fake::allocations==0);
 std::puts("IT8951: native16 packing, transitions, overlay, OOM, cancellation, HRDY/LUT timeouts and recovery passed");
}
'''
with tempfile.TemporaryDirectory(prefix='freeink-it8951-') as directory:
    root=Path(directory)
    for name,source in STUBS.items():
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(source)
    for name in ('src/driver/It8951Driver.h','src/driver/It8951Driver.cpp','src/driver/PanelDriver.h',
                 'include/GrayscaleCapabilities.h','include/RefreshContext.h'):
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(LIB/name,path)
    (root/'test.cpp').write_text(HARNESS)
    executable=root/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-Wno-unused-parameter','-fsanitize=address,undefined','-I'+str(root),'-I'+str(root/'include'),
                    str(root/'test.cpp'),str(root/'src/driver/It8951Driver.cpp'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
