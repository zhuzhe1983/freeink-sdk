"""Compile the real LGFX driver against a recording M5GFX boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile

LIB = Path(__file__).resolve().parents[2]
STUB = r'''#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <cassert>
inline bool failCanvas=false, externalRam=true;
inline unsigned pushes=0, waits=0, sleeps=0;
inline std::vector<uint8_t> shown;
namespace lgfx {
namespace epd_mode {enum epd_mode_t {epd_quality,epd_text,epd_fast,epd_fastest};}
inline epd_mode::epd_mode_t mode=epd_mode::epd_fast;
enum class color_depth_t {grayscale_8bit};
class Bus_EPD {
 public:
 struct Config {int bus_speed=0,pin_data[8]{},pin_pwr=0,pin_sph=0,pin_spv=0,pin_oe=0,pin_le=0,pin_cl=0,pin_ckv=0,bus_width=0;};
 Config config(){return {};}; void config(Config){};
 virtual bool init(){return true;} virtual bool powerControl(bool){return true;} void wait(){}
 protected: bool _pwr_on=false;
};
class Panel_EPD {
 public:
 struct Detail {int line_padding=0; const uint32_t *lut_quality=nullptr,*lut_text=nullptr,*lut_fast=nullptr,*lut_fastest=nullptr; size_t lut_quality_step=0,lut_text_step=0,lut_fast_step=0,lut_fastest_step=0;};
 struct Config {int memory_width=0,panel_width=0,memory_height=0,panel_height=0,offset_rotation=0,offset_x=0,offset_y=0;bool bus_shared=false;};
 void setBus(Bus_EPD*){} Detail config_detail(){return {};} void config_detail(Detail){} Config config(){return {};}void config(Config){}
};
class LGFX_Device {
 public:
 void setPanel(Panel_EPD*){} void init(){} void setRotation(int){} void setEpdMode(epd_mode::epd_mode_t v){mode=v;}
 void waitDisplay(){++waits;} void sleep(){++sleeps;} void setAutoDisplay(bool){} void display(){}
 int color888(int,int,int){return 0;} void fillScreen(int){}
};
class LGFX_Sprite {
 std::vector<uint8_t> data;
 public:
 explicit LGFX_Sprite(LGFX_Device*){} void setPsram(bool){} void setColorDepth(color_depth_t){}
 void createSprite(int w,int h){if(!failCanvas)data.resize(w*h);}
 void* getBuffer(){return data.empty()?nullptr:data.data();}
 void pushSprite(int,int){++pushes;shown=data;}
};
}
'''
TEST = r'''
#include "src/driver/LgfxEpdDriver.h"
#include <M5GFX.h>
#include <cstring>
#include <string>
namespace freeink { const LgfxEpdConfig& testConfig(){static const LgfxEpdConfig cfg{};return cfg;} }
int main(int argc,char** argv){
 assert(argc==2); const std::string scenario=argv[1];
 failCanvas=scenario=="oom";externalRam=scenario!="internal";
 freeink::EpdBus bus;auto& d=freeink::lgfxEpdDriver();d.begin(bus);
 uint8_t bw[4]={0xAA,0x55,0xF0,0x0F};
 if(failCanvas || !externalRam){assert(!d.beginGrayscale16());assert(!d.commitGrayscale16(bw));d.display(bus,bw,nullptr,freeink::RefreshMode::Fast,false);assert(pushes==0);return 0;}
 if(BOARD_GRAY==4){assert(!d.beginGrayscale16());d.display(bus,bw,nullptr,freeink::RefreshMode::Half,false);assert(pushes==1);return 0;}
 for(unsigned repeat=0;repeat<3;++repeat){
   auto* p=d.beginGrayscale16();assert(p && !d.beginGrayscale16());
   for(unsigned i=0;i<16;++i){assert(p[i]==255);p[i]=uint8_t((i%8)*2 | ((i%8)*2+1)<<4);}
   assert(d.commitGrayscale16(bw));assert(!d.commitGrayscale16(bw));
   assert(shown.size()==32 && lgfx::mode==lgfx::epd_mode::epd_quality);
   for(unsigned i=0;i<32;++i)assert(shown[i]==(i%16)*17);
 }
 auto* p=d.beginGrayscale16();assert(p);memset(p,0,16);const unsigned before=pushes;
 d.cancelGrayscale16();d.cancelGrayscale16();d.displayGray(bus,bw,false,nullptr,false);assert(pushes==before);
 d.display(bus,bw,nullptr,freeink::RefreshMode::Full,true);
 assert(pushes==before+1 && lgfx::mode==lgfx::epd_mode::epd_text);
 for(unsigned i=0;i<32;++i)assert(shown[i]==((bw[i/8]>>(7-i%8)&1)?255:0));
 uint8_t lsb[4]={0x80,0,0,0},msb[4]={0xC0,0,0,0};
 d.copyGrayscaleLsb(bus,lsb);d.copyGrayscaleMsb(bus,msb);d.displayGray(bus,bw,false,nullptr,false);
 assert(shown[0]==0x55 && shown[1]==0xAA && lgfx::mode==lgfx::epd_mode::epd_text);
 assert(d.beginGrayscale16());d.deepSleep(bus);assert(!d.commitGrayscale16(bw));
 assert(waits>=pushes*2 && sleeps>=4);
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='lgfx-native-') as directory:
        root=Path(directory)
        shutil.copytree(LIB/'src',root/'src')
        shutil.copytree(LIB/'include',root/'include')
        (root/'Arduino.h').write_text('#pragma once\n#include <cstdint>\n#include <cstddef>\nstruct SerialStub {void printf(const char*,...) {}};inline SerialStub Serial;\n')
        (root/'BoardConfig.h').write_text('#pragma once\nnamespace BoardConfig { struct Profile {unsigned displayWidth=16,displayHeight=2,grayscaleLevels=BOARD_GRAY;};inline constexpr Profile ACTIVE{};}\n')
        (root/'src/bus/EpdBus.h').write_text('#pragma once\nnamespace freeink {enum class BusyPolarity {ActiveLow,ActiveHigh};class EpdBus{};}\n')
        (root/'M5GFX.h').write_text(STUB)
        (root/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\ninline void* heap_caps_malloc(size_t n,int){return std::malloc(n);}\n')
        (root/'esp_memory_utils.h').write_text('#pragma once\n#include <M5GFX.h>\ninline bool esp_ptr_external_ram(void*){return externalRam;}\n')
        for name in ('Bus_EPD.h','Panel_EPD.hpp'):
            p=root/'lgfx/v1/platforms/esp32'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include <M5GFX.h>\n')
        (root/'test.cpp').write_text(TEST)
        for gray in (16,4):
            binary=root/f'test{gray}'
            subprocess.run(['c++','-std=c++17','-fno-exceptions','-Wall','-Wextra','-Werror','-Wno-unused-parameter',f'-DBOARD_GRAY={gray}','-DFREEINK_DRIVER_LGFX_EPD=1','-DFREEINK_LGFX_EPD_CONFIG=testConfig','-I'+str(root),'-I'+str(root/'include'),str(root/'test.cpp'),str(root/'src/driver/LgfxEpdDriver.cpp'),'-o',str(binary)],check=True)
            for scenario in ('normal','oom','internal'):
                subprocess.run([str(binary),scenario],check=True)
    print('LGFX native/overlay/rollback and allocation-failure tests passed')

if __name__=='__main__':main()
