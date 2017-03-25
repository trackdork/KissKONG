/*
  Copyright (c) 2017 Hristo Gochkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "KissAndMsp.h"
#include "MAX7456.h"

// uncomment this for specific OSD board here
// https://droneplastics.com/reaktor-pdbosd
//#define REAKTOR_PDB_OSD

// uncomment this to turn on the crosshair
//#define USE_CROSSHAIR

#ifdef REAKTOR_PDB_OSD
  // MAX7456 chip is connected on pin D6 (pin 5)
  #define SPI_PIN 5
  // RESET must be pulled high (pin 10)
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
#else
  #define SPI_PIN 10
#endif

MAX7456 osd(SPI_PIN);// MAX7456 attached to SPI and SS pin 10

/*
 * MSP implementation for Font Upload
 * */

void onMSP(msp_direction_t direction, bool err, uint8_t cmd, uint8_t * data, uint8_t len){
    if(cmd == MSP_OSD){
        if(data[0] == MSP_OSD_FONT && direction == APP_TO_DEVICE && !err){
            if(len == 56 || len == 64){
                osd.writeFontChar(data[55], &data[1]);
            }
            return;
        }
    }
}

void msp_delay(uint32_t ms){
    uint32_t _now = millis();
    uint32_t _end = _now + ms;
    if(_now > _end){
        while(millis() > _end){
            msp_update();
        }
    }
    while(millis() < _end){
        msp_update();
    }
}

// Table drawing chars (in the OSD font)
#define TB_HL   "\x01"
#define TB_VL   "\x02"
#define TB_TL   "\x03"
#define TB_TR   "\x04"
#define TB_BL   "\x05"
#define TB_BR   "\x06"
#define TB_LM   "\x07"
#define TB_RM   "\x08"
#define TB_TM   "\x09"
#define TB_BM   "\x0b"
#define TB_XC   "\x0c"

#define TB_AU   "\x0e"
#define TB_AD   "\x0f"
#define TB_AL   "\x89"
#define TB_AR   "\x88"

#define CROSSHAIR "\xd0" "\xd1"

#define ARROW_LEFT   0x89
#define ARROW_RIGHT  0x88

#define PID_TABLE_TOP " " TB_TL TB_HL TB_HL TB_HL TB_HL TB_HL TB_TM TB_HL TB_HL TB_HL TB_HL TB_HL TB_TM TB_HL TB_HL TB_HL TB_HL TB_HL TB_TM TB_HL TB_HL TB_HL TB_HL TB_HL TB_TR
#define PID_TABLE_MID " " TB_LM TB_HL TB_HL TB_HL TB_HL TB_HL TB_XC TB_HL TB_HL TB_HL TB_HL TB_HL TB_XC TB_HL TB_HL TB_HL TB_HL TB_HL TB_XC TB_HL TB_HL TB_HL TB_HL TB_HL TB_RM
#define PID_TABLE_BOT " " TB_BL TB_HL TB_HL TB_HL TB_HL TB_HL TB_BM TB_HL TB_HL TB_HL TB_HL TB_HL TB_BM TB_HL TB_HL TB_HL TB_HL TB_HL TB_BM TB_HL TB_HL TB_HL TB_HL TB_HL TB_BR
#define PID_TABLE_ROW " " TB_VL "     " TB_VL "     " TB_VL "     " TB_VL "     " TB_VL

// Sicks Axis for actions
#define AXIS_LR telemetry.roll  //Left+Right
#define AXIS_UD telemetry.pitch //Up+Down
#define AXIS_YN telemetry.yaw   //Yes(Add)+No(Substract)

#define INCREMENT_VALUE(i,v,m) i = ((i + (v)) % (m))
#define DECREMENT_VALUE(i,v,m) i = (i >= (v))?(i - (v)):(i + (m) - (v))

#define INCREMENT_IVALUE(i,v,m) i = ((i + (v)) < (m))?(i + (v)):(m)
#define DECREMENT_IVALUE(i,v,m) i = ((i - (v)) > (m))?(i - (v)):(m)

typedef void (view_cb_t)(void);

// View Methods
typedef struct {
        view_cb_t * init;
        view_cb_t * update;
        view_cb_t * deinit;
} view_methods_t;

// Combined ESC Statistics
typedef struct {
        uint8_t count;
        int32_t temperature;
        int32_t voltage;// /100
        int32_t current;// /100
        int32_t used_ah;// /1000
        int32_t rpm; // (v*32)/MAGNETPOLECOUNT
} kiss_esc_stats_t;

// Flight Statistics
typedef struct {
        uint32_t duration;//ms duration
        uint8_t min_signal;
        int16_t min_voltage;
        int16_t max_current;
        int16_t max_rpm;
        int16_t max_temperature;
} flight_stats_t;

// Position struct to hold coordinates
typedef struct {
        uint8_t x;
        uint8_t y;
} osd_pos_t;

// Views
typedef enum {
    VIEW_SETTINGS, VIEW_PIDS, VIEW_RATES, VIEW_XPIDS, VIEW_FILTERS, VIEW_INFO, VIEW_LAST_FLIGHT
    , VIEW_OFF, VIEW_MAIN, VIEW_STATS, VIEW_BAD
} view_index_t;

// Stick Combinations
typedef enum {
    STICKS_NONE, STICKS_LEFT, STICKS_RIGHT, STICKS_UP, STICKS_DOWN, STICKS_YES, STICKS_NO, STICKS_MENU, STICKS_ERR
} sticks_value_t;

typedef enum {
    LED_BLACK, LED_WHITE, LED_YELLOW, LED_ORANGE, LED_RED, LED_PURPLE, LED_BLUE, LED_GREEN, LED_CYAN, LED_CUSTOM
} kiss_led_colors_t;

const kiss_led_color_t kiss_led_colors[LED_CUSTOM] = {
    {0,0,0},
    {255,255,255},
    {255,255,0},
    {255,123,0},
    {255,0,0},
    {128,0,128},
    {0,0,255},
    {0,255,0},
    {0,255,255}
};

//
static int8_t offsetLeft = 18;
static int8_t offsetTop = 13;

static view_index_t currentView = VIEW_OFF;
static view_index_t lastView = VIEW_OFF;
static uint32_t lastViewSwitch = 0;

static uint8_t mainMenuIndex = 0;

static sticks_value_t currentSticks = STICKS_ERR;

static flight_stats_t flight_stats = {0,0,0,0,0,0};
static kiss_esc_stats_t esc_stats = {0,0,0,0,0,0};
static kiss_telemetry_t telemetry;
static kiss_settings_t settings;
static kiss_esc_info_t info;
static char name[17] = {0};
static bool update_settings = true;
static bool update_info = true;

//Save current settings
bool kiss_save(){
    if(!kiss_set_settings(&settings)) {
        return false;
    }
    return true;
}

//Update telemetry, info and configuration
bool kiss_update(){
    if(!kiss_get_telemetry(&telemetry)){
        return false;
    }
    update_esc_stats();
    if(telemetry.armed){
        return true;
    }
    if(update_info){
        if(!kiss_get_info(name, &info)){
            return false;
        }
        update_info = false;
    }
    if(update_settings){
        if(!kiss_get_settings(&settings)) {
            return false;
        }
        update_settings = false;
    }
    return true;
}

/*
 * Combined ESC Stats Collection
 * */

void update_esc_stats(){
    esc_stats.count = 0;
    esc_stats.temperature = 0;
    esc_stats.voltage = 0;
    esc_stats.current = 0;
    esc_stats.used_ah = 0;
    esc_stats.rpm = 0;
    uint8_t i;
    for(i=0; i<6; i++){
        if(telemetry.esc[i].voltage < 6){
            continue;
        }
        esc_stats.count++;
        esc_stats.temperature += telemetry.esc[i].temperature;
        esc_stats.voltage += telemetry.esc[i].voltage;
        esc_stats.current += telemetry.esc[i].current;
        esc_stats.used_ah += telemetry.esc[i].used_ah;
        esc_stats.rpm += telemetry.esc[i].rpm;
    }
    if(esc_stats.count){
        esc_stats.temperature /= esc_stats.count;
        esc_stats.voltage /= esc_stats.count;
        if(telemetry.armed){
            esc_stats.rpm /= esc_stats.count;
        } else {
            esc_stats.rpm = 0;//fix RPMs being reported when disarmed
        }
    }
}

/*
 * Flight Stats Collection
 * */

static void startStats(){
    memset((uint8_t*)&flight_stats, 0, sizeof(flight_stats_t));
    flight_stats.duration = millis();
    flight_stats.min_voltage = 4000;
    flight_stats.min_signal = 100;
}

static void collectStats(){
    int16_t voltage = esc_stats.count?esc_stats.voltage:telemetry.voltage;
    if(voltage < flight_stats.min_voltage){
        flight_stats.min_voltage = voltage;
    }
    if((100 - telemetry.failsafe) < flight_stats.min_signal){
        flight_stats.min_signal = (100 - telemetry.failsafe);
    }
    if(!esc_stats.count){
        return;
    }
    if(esc_stats.current > flight_stats.max_current){
        flight_stats.max_current = esc_stats.current;
    }
    if(esc_stats.rpm > flight_stats.max_rpm){
        flight_stats.max_rpm = esc_stats.rpm;
    }
    if(esc_stats.temperature > flight_stats.max_temperature){
        flight_stats.max_temperature = esc_stats.temperature;
    }
}

static void stopStats(){
    flight_stats.duration = millis() - flight_stats.duration;
}

/*
 * Helpers
 * */

//get max current that KISS ESC supports
int16_t getEscMaxCurrent(uint8_t index){
    if(index >= info.count || !info.esc[index].type){
        return 0;
    }
    if(info.esc[index].type == 1){
        return 800;//8A
    } else if(info.esc[index].type == 2){
        return 1600;//16A
    } else if(info.esc[index].type == 3){
        return 2400;//24A
    }
    return 0;
}

//Detects if Arm switch is set
static bool hasArmSwitch(){
    return ((settings.aux[AUX_ARM].channel != AUX_NONE) && (settings.aux[AUX_ARM].level != AUX_OFF));
}

// Reads the stick values from telemetry and determines if a combination is active
static uint8_t sticksGetValue(){
    static sticks_value_t sticksLastValue = STICKS_ERR;
    static sticks_value_t sticksValueBeforeLast = STICKS_ERR;
    if(!KISS_THROTTLE_IS_MID(telemetry.throttle)){
        sticksLastValue = STICKS_ERR;
        return sticksLastValue;
    }
    sticks_value_t sticks = STICKS_ERR;
    if(KISS_AUX_IS_MID(AXIS_YN)){
        if(KISS_AUX_IS_MID(AXIS_UD)){
            if(KISS_AUX_IS_LOW(AXIS_LR)){
                sticks = STICKS_LEFT;
            } else if(KISS_AUX_IS_HIGH(AXIS_LR)){
                sticks = STICKS_RIGHT;
            } else {
                sticks = STICKS_NONE;
            }
        } else if(KISS_AUX_IS_HIGH(AXIS_UD)){
            sticks = STICKS_UP;
        } else {
            sticks = STICKS_DOWN;
        }
    } else if(KISS_AUX_IS_LOW(AXIS_YN) && KISS_AUX_IS_MID(AXIS_UD) && KISS_AUX_IS_MID(AXIS_LR)){
        sticks = STICKS_NO;
    } else if(KISS_AUX_IS_MID(AXIS_UD)){
        if(KISS_AUX_IS_MID(AXIS_LR)){
            sticks = STICKS_YES;
        } else if(KISS_AUX_IS_LOW(AXIS_LR)){
            sticks = STICKS_MENU;
        }
    }
    if(sticks == sticksLastValue && sticks == sticksValueBeforeLast){
        return sticksLastValue;
    }
    sticksValueBeforeLast = sticksLastValue;
    sticksLastValue = sticks;
    return STICKS_ERR;
}

// Swich the current view
static void setCurrentView(uint8_t view){
    currentView = (view_index_t)view;
    lastViewSwitch = millis();
}

// when SAVE is selected in a settings screen
static void settingsSave(){
    if(kiss_save()){
        update_settings = true;
        setCurrentView(VIEW_MAIN);
    } else {
        //show some error?
    }
}

// when CANCEL is selected in a settings screen
static void settingsRestore(){
    update_settings = true;
    setCurrentView(VIEW_MAIN);
}

static uint8_t getColorIndex(){
    uint8_t i;
    for(i=0;i<LED_CUSTOM;i++){
        if(settings.rgb.r == kiss_led_colors[i].r && settings.rgb.g == kiss_led_colors[i].g && settings.rgb.b == kiss_led_colors[i].b){
            break;
        }
    }
    return i;
}

static void setColorIndex(uint8_t index){
    if(index >= LED_CUSTOM){
        return;
    }
    settings.rgb.r = kiss_led_colors[index].r;
    settings.rgb.g = kiss_led_colors[index].g;
    settings.rgb.b = kiss_led_colors[index].b;
}

/*
 * Settings View
 * */

static int16_t * settingsValues[4] = {
        &settings.min_throttle,
        &settings.max_throttle,
        &settings.min_command,
        &settings.mid_command
};

#define SETTINGS_VIEW_NUM_INDEXES 9
static uint8_t settings_view_current_index = 0;
const osd_pos_t settings_view_positions[SETTINGS_VIEW_NUM_INDEXES] = {
           {16,3}, //Min Throttle
           {16,4}, //Max Throttle
           {16,5}, //Min Command
           {16,6}, //Mid Command
           {16,7}, //Min Voltage
           {16,8}, //ESC Mode
           {16,9}, //Led Color
        {4,10}, {17,10}
};

static void drawSettings(){
    osd.setCursor(9,1);  osd.print(F("Settings"));
    osd.setCursor(2,2);  osd.print(F(TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL));
    osd.setCursor(2,3);  osd.print(F("Min Throttle:"));
    osd.setCursor(2,4);  osd.print(F("Max Throttle:"));
    osd.setCursor(2,5);  osd.print(F("Min Command :"));
    osd.setCursor(2,6);  osd.print(F("Mid Command :"));
    osd.setCursor(2,7);  osd.print(F("Min Voltage :"));
    osd.setCursor(2,8);  osd.print(F("ESC Protocol:"));
    osd.setCursor(2,9);  osd.print(F("LED Color   :"));
    osd.setCursor(5,10); osd.print(F("CANCEL"));
    osd.setCursor(18,10); osd.print(F("SAVE"));
    settings_view_current_index = 0;
}

static void updateSettings(){
    if(currentSticks && currentSticks < STICKS_ERR){
        if(currentSticks == STICKS_DOWN){
            INCREMENT_VALUE(settings_view_current_index, 1, SETTINGS_VIEW_NUM_INDEXES);
        } else if(currentSticks == STICKS_UP){
            DECREMENT_VALUE(settings_view_current_index, 1, SETTINGS_VIEW_NUM_INDEXES);
        } else if(settings_view_current_index == (SETTINGS_VIEW_NUM_INDEXES - 2) && currentSticks == STICKS_YES){
            settingsRestore();
        } else if(settings_view_current_index == (SETTINGS_VIEW_NUM_INDEXES - 1) && currentSticks == STICKS_YES){
            settingsSave();
        } else if(currentSticks == STICKS_YES){
            if(settings_view_current_index < 4){
                INCREMENT_VALUE(*settingsValues[settings_view_current_index], 1, 1001);
            } else if(settings_view_current_index == 4){
                INCREMENT_VALUE(settings.vbat_alarm, 1, 300);
            } else if(settings_view_current_index == 5){
                INCREMENT_VALUE(settings.oneshot_125, 1, 6);
            } else if(settings_view_current_index == 6){
                uint8_t c = getColorIndex();
                INCREMENT_VALUE(c, 1, LED_CUSTOM);
                setColorIndex(c);
            }
        } else if(currentSticks == STICKS_NO){
            if(settings_view_current_index < 4){
                DECREMENT_VALUE(*settingsValues[settings_view_current_index], 1, 1001);
            } else if(settings_view_current_index == 4){
                DECREMENT_VALUE(settings.vbat_alarm, 1, 300);
            } else if(settings_view_current_index == 5){
                DECREMENT_VALUE(settings.oneshot_125, 1, 6);
            } else if(settings_view_current_index == 6){
                uint8_t c = getColorIndex();
                DECREMENT_VALUE(c, 1, LED_CUSTOM);
                setColorIndex(c);
            }
        }
    }

    uint8_t i;
    for(i=0; i<SETTINGS_VIEW_NUM_INDEXES; i++){
        osd.setCursor(settings_view_positions[i].x,settings_view_positions[i].y);
        osd.write((settings_view_current_index == i)?ARROW_RIGHT:' ');
    }

    osd.setCursor(19,3); printMah(*settingsValues[0] + 1000, false);
    osd.setCursor(19,4); printMah(*settingsValues[1] + 1000, false);
    osd.setCursor(19,5); printMah(*settingsValues[2] + 1000, false);
    osd.setCursor(19,6); printMah(*settingsValues[3] + 1000, false);
    osd.setCursor(18,7); printVoltage(settings.vbat_alarm * 10, false);
    osd.setCursor(18,8); printOutputMode();
    osd.setCursor(17,9); printLedColor();
}

/*
 * Filters View
 * */

#define FILTERS_VIEW_NUM_INDEXES 7
static uint8_t filters_view_current_index = 0;
const osd_pos_t filters_view_positions[FILTERS_VIEW_NUM_INDEXES] = {
           {16,3}, //LPF
           {16,5}, //Notch Enable
           {16,6}, //Notch Center Freqency
           {16,7}, //Notch Cutoff Frequency
           {16,9}, //Yaw Strength
        {4,10}, {17,10}
};

static void drawFilters(){
    osd.setCursor(10,1);  osd.print(F("Filters"));
    osd.setCursor(2,2);  osd.print(F(TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL));
    osd.setCursor(2,3);  osd.print(F("LPF Freq    :"));
    osd.setCursor(8,4);  osd.print(F("Notch Filter"));
    osd.setCursor(2,5);  osd.print(F("Enabled     :"));
    osd.setCursor(2,6);  osd.print(F("Center Freq :"));
    osd.setCursor(2,7);  osd.print(F("Cutoff Freq :"));
    osd.setCursor(9,8);  osd.print(F("Yaw Filter"));
    osd.setCursor(2,9);  osd.print(F("Yaw Strength:"));
    osd.setCursor(5,10); osd.print(F("CANCEL"));
    osd.setCursor(18,10); osd.print(F("SAVE"));
    filters_view_current_index = 0;
}

static void updateFilters(){
    if(currentSticks && currentSticks < STICKS_ERR){
        if(currentSticks == STICKS_DOWN){
            INCREMENT_VALUE(filters_view_current_index, 1, FILTERS_VIEW_NUM_INDEXES);
        } else if(currentSticks == STICKS_UP){
            DECREMENT_VALUE(filters_view_current_index, 1, FILTERS_VIEW_NUM_INDEXES);
        } else if(filters_view_current_index == (FILTERS_VIEW_NUM_INDEXES - 2) && currentSticks == STICKS_YES){
            settingsRestore();
        } else if(filters_view_current_index == (FILTERS_VIEW_NUM_INDEXES - 1) && currentSticks == STICKS_YES){
            settingsSave();
        } else if(currentSticks == STICKS_YES){
            if(filters_view_current_index == 0){
                INCREMENT_VALUE(settings.lpf, 1, 7);
            } else if(filters_view_current_index == 1){
                settings.notch_filter_enable = !settings.notch_filter_enable;
            } else if(filters_view_current_index == 2){
                INCREMENT_VALUE(settings.notch_filter_center, 1, 1001);
            } else if(filters_view_current_index == 3){
                INCREMENT_VALUE(settings.notch_filter_cut, 1, 1001);
            } else if(filters_view_current_index == 4){
                INCREMENT_VALUE(settings.yaw_c_filter, 1, 98);
            }
        } else if(currentSticks == STICKS_NO){
            if(filters_view_current_index == 0){
                DECREMENT_VALUE(settings.lpf, 1, 7);
            } else if(filters_view_current_index == 1){
                settings.notch_filter_enable = !settings.notch_filter_enable;
            } else if(filters_view_current_index == 2){
                DECREMENT_VALUE(settings.notch_filter_center, 1, 1001);
            } else if(filters_view_current_index == 3){
                DECREMENT_VALUE(settings.notch_filter_cut, 1, 1001);
            } else if(filters_view_current_index == 4){
                DECREMENT_VALUE(settings.yaw_c_filter, 1, 98);
            }
        }
    }

    uint8_t i;
    for(i=0; i<FILTERS_VIEW_NUM_INDEXES; i++){
        osd.setCursor(filters_view_positions[i].x,filters_view_positions[i].y);
        osd.write((filters_view_current_index == i)?ARROW_RIGHT:' ');
    }

    osd.setCursor(17,3); printLpfFreq();
    osd.setCursor(19,5); settings.notch_filter_enable?osd.print(F("YES")):osd.print(F(" NO"));
    osd.setCursor(18,6); printMah(settings.notch_filter_center, false);
    osd.setCursor(18,7); printMah(settings.notch_filter_cut, false);
    osd.setCursor(18,9); printMah(settings.yaw_c_filter, false);
}

/*
 * TPA and Level PIDs Table
 * */

#define TPA_PID_TABLE_NUM_INDEXES 9
static uint8_t tpa_pid_table_current_index = 0;
const osd_pos_t tpa_pid_table_positions[TPA_PID_TABLE_NUM_INDEXES] = {
    {8,4}, {14,4}, {20,4},  //TPA PIDs
    {8,6}, {14,6}, {20,6},  //Angle PIDs
           {16,8},          //Max Angle
        {4,9}, {17,9}
};

static uint16_t * xtraPidValues[7] = {
        &settings.tpa[0],
        &settings.tpa[1],
        &settings.tpa[2],
        &settings.angle_p,
        &settings.angle_i,
        &settings.angle_d,
        &settings.max_angle
};

static void drawXtraPids(){
    osd.setCursor(0,1);
    osd.println(F(PID_TABLE_TOP));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_MID));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_MID));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_BOT));

    osd.setCursor(10,2); osd.print(F("P"));
    osd.setCursor(16,2); osd.print(F("I"));
    osd.setCursor(22,2); osd.print(F("D"));
    osd.setCursor(2,4);  osd.print(F("TPA"));
    osd.setCursor(2,6);  osd.print(F("ANGLE"));
    osd.setCursor(3,8);  osd.print(F("MAX ANGLE :"));
    osd.setCursor(5,9); osd.print(F("CANCEL"));
    osd.setCursor(18,9); osd.print(F("SAVE"));
    tpa_pid_table_current_index = 0;
}

static void updateXtraPids(){
    if(currentSticks && currentSticks < STICKS_ERR){
        if(currentSticks == STICKS_LEFT){
            DECREMENT_VALUE(tpa_pid_table_current_index, 1, TPA_PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_RIGHT){
            INCREMENT_VALUE(tpa_pid_table_current_index, 1, TPA_PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_DOWN){
            INCREMENT_VALUE(tpa_pid_table_current_index, (tpa_pid_table_current_index>5)?1:3, TPA_PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_UP){
            DECREMENT_VALUE(tpa_pid_table_current_index, (tpa_pid_table_current_index<7)?3:1, TPA_PID_TABLE_NUM_INDEXES);
        } else if(tpa_pid_table_current_index == (TPA_PID_TABLE_NUM_INDEXES - 2) && currentSticks == STICKS_YES){
            settingsRestore();
        } else if(tpa_pid_table_current_index == (TPA_PID_TABLE_NUM_INDEXES - 1) && currentSticks == STICKS_YES){
            settingsSave();
        } else if(currentSticks == STICKS_YES){
            if(tpa_pid_table_current_index == 6){
                INCREMENT_VALUE(*xtraPidValues[6], 14, 2574);
            } else {
                INCREMENT_VALUE(*xtraPidValues[tpa_pid_table_current_index], 100, 10000);
            }
        } else if(currentSticks == STICKS_NO){
            if(tpa_pid_table_current_index == 6){
                DECREMENT_VALUE(*xtraPidValues[6], 14, 2574);
            } else {
                DECREMENT_VALUE(*xtraPidValues[tpa_pid_table_current_index], 100, 10000);
            }
        }
    }

    uint8_t i;
    for(i=0; i<TPA_PID_TABLE_NUM_INDEXES; i++){
        osd.setCursor(tpa_pid_table_positions[i].x,tpa_pid_table_positions[i].y);
        osd.write((tpa_pid_table_current_index == i)?ARROW_RIGHT:' ');
    }

    osd.setCursor(9,4);  printPid(*xtraPidValues[0]/100);
    osd.setCursor(15,4); printPid(*xtraPidValues[1]/100);
    osd.setCursor(21,4); printPid(*xtraPidValues[2]/100);
    osd.setCursor(9,6);  printPid(*xtraPidValues[3]/100);
    osd.setCursor(15,6); printPid(*xtraPidValues[4]/100);
    osd.setCursor(21,6); printPid(*xtraPidValues[5]/100);
    osd.setCursor(18,8); printMaxAngle(*xtraPidValues[6]/14.3);
}

/*
 * PIDs and Rates View
 * */

#define PID_TABLE_NUM_INDEXES 11
static uint8_t pid_table_current_index = 0;
const osd_pos_t pid_table_positions[PID_TABLE_NUM_INDEXES] = {
    {8,4}, {14,4}, {20,4},
    {8,6}, {14,6}, {20,6},
    {8,8}, {14,8}, {20,8},
        {4,10}, {17,10}
};

static void drawPidTable(){
    osd.setCursor(0,1);
    osd.println(F(PID_TABLE_TOP));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_MID));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_MID));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_MID));
    osd.println(F(PID_TABLE_ROW));
    osd.println(F(PID_TABLE_BOT));
    osd.setCursor(5,10); osd.print(F("CANCEL"));
    osd.setCursor(18,10); osd.print(F("SAVE"));
    pid_table_current_index = 0;
}

static void updatePidTableIndex(){
    if(currentSticks && currentSticks < STICKS_ERR){
        if(currentSticks == STICKS_LEFT){
            DECREMENT_VALUE(pid_table_current_index, 1, PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_RIGHT){
            INCREMENT_VALUE(pid_table_current_index, 1, PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_DOWN){
            INCREMENT_VALUE(pid_table_current_index, 3, PID_TABLE_NUM_INDEXES);
        } else if(currentSticks == STICKS_UP){
            DECREMENT_VALUE(pid_table_current_index, 3, PID_TABLE_NUM_INDEXES);
        } else if(pid_table_current_index == (PID_TABLE_NUM_INDEXES - 2) && currentSticks == STICKS_YES){
            settingsRestore();
        } else if(pid_table_current_index == (PID_TABLE_NUM_INDEXES - 1) && currentSticks == STICKS_YES){
            settingsSave();
        }
    }

    uint8_t i;
    for(i=0; i<PID_TABLE_NUM_INDEXES; i++){
        osd.setCursor(pid_table_positions[i].x,pid_table_positions[i].y);
        osd.write((pid_table_current_index == i)?ARROW_RIGHT:' ');
    }
}

/*
 * PIDs View
 * */

static uint16_t * pidsValues[9] = {
        &settings.pid_p[0],
        &settings.pid_i[0],
        &settings.pid_d[0],
        &settings.pid_p[1],
        &settings.pid_i[1],
        &settings.pid_d[1],
        &settings.pid_p[2],
        &settings.pid_i[2],
        &settings.pid_d[2]
};

static void drawPids(){
    drawPidTable();

    osd.setCursor(10,2); osd.print(F("P"));
    osd.setCursor(16,2); osd.print(F("I"));
    osd.setCursor(22,2); osd.print(F("D"));
    osd.setCursor(2,4); osd.print(F("ROLL"));
    osd.setCursor(2,6); osd.print(F("PITCH"));
    osd.setCursor(2,8); osd.print(F("YAW"));

}

static void updatePids(){
    updatePidTableIndex();

    if(pid_table_current_index < 9){
        if(currentSticks == STICKS_YES){
            if(pid_table_current_index == 1 || pid_table_current_index == 4 || pid_table_current_index == 7){
                INCREMENT_VALUE(*pidsValues[pid_table_current_index], 1, 100);
            } else {
                INCREMENT_VALUE(*pidsValues[pid_table_current_index], 100, 10000);
            }
        } else if(currentSticks == STICKS_NO){
            if(pid_table_current_index == 1 || pid_table_current_index == 4 || pid_table_current_index == 7){
                DECREMENT_VALUE(*pidsValues[pid_table_current_index], 1, 100);
            } else {
                DECREMENT_VALUE(*pidsValues[pid_table_current_index], 100, 10000);
            }
        }
    }

    osd.setCursor(9,4);  printPid(*pidsValues[0]/100);
    osd.setCursor(15,4); printPid(*pidsValues[1]*10);
    osd.setCursor(21,4); printPid(*pidsValues[2]/100);
    osd.setCursor(9,6);  printPid(*pidsValues[3]/100);
    osd.setCursor(15,6); printPid(*pidsValues[4]*10);
    osd.setCursor(21,6); printPid(*pidsValues[5]/100);
    osd.setCursor(9,8);  printPid(*pidsValues[6]/100);
    osd.setCursor(15,8); printPid(*pidsValues[7]*10);
    osd.setCursor(21,8); printPid(*pidsValues[8]/100);
}

/*
 * Rates View
 * */

static int16_t * ratesValues[9] = {
        &settings.rc_rate[0],
        &settings.rc_expo[0],
        &settings.rc_curve[0],
        &settings.rc_rate[1],
        &settings.rc_expo[1],
        &settings.rc_curve[1],
        &settings.rc_rate[2],
        &settings.rc_expo[2],
        &settings.rc_curve[2]
};

static void drawRates(){
    drawPidTable();

    osd.setCursor(8,2); osd.print(F("RATE"));
    osd.setCursor(14,2); osd.print(F("EXPO"));
    osd.setCursor(20,2); osd.print(F("CURVE"));
    osd.setCursor(2,4); osd.print(F("ROLL"));
    osd.setCursor(2,6); osd.print(F("PITCH"));
    osd.setCursor(2,8); osd.print(F("YAW"));
}

static void updateRates(){
    updatePidTableIndex();

    if(pid_table_current_index < 9){
        if(currentSticks == STICKS_YES){
            INCREMENT_VALUE(*ratesValues[pid_table_current_index], 10, 1000);
        } else if(currentSticks == STICKS_NO){
            DECREMENT_VALUE(*ratesValues[pid_table_current_index], 10, 1000);
        }
    }

    osd.setCursor(9,4);  printRate(*ratesValues[0]/10);
    osd.setCursor(15,4); printRate(*ratesValues[1]/10);
    osd.setCursor(21,4); printRate(*ratesValues[2]/10);
    osd.setCursor(9,6);  printRate(*ratesValues[3]/10);
    osd.setCursor(15,6); printRate(*ratesValues[4]/10);
    osd.setCursor(21,6); printRate(*ratesValues[5]/10);
    osd.setCursor(9,8);  printRate(*ratesValues[6]/10);
    osd.setCursor(15,8); printRate(*ratesValues[7]/10);
    osd.setCursor(21,8); printRate(*ratesValues[8]/10);
}

/*
 * Board Info View
 * */

static void drawInfo(){
    uint8_t i, line = 1;

    static uint8_t lastEscCount = esc_stats.count;
    if(esc_stats.count != lastEscCount){
        lastEscCount = esc_stats.count;
        for(i=line;i<11;i++){
            osd.clearLine(i);
        }
    }
    //FC Version
    osd.setCursor((28 - (strlen(name)+6))/2,line++);
    //frame type?
    printFrameType();
    osd.write(' ');
    osd.print(name);

    //Found ESCs
    for(i=0;i<esc_stats.count; i++){
        if(i&1){
            //escs 1, 3 and 5
            osd.setCursor(14, line++);
        } else if(i == (esc_stats.count - 1)){
            //esc 2 on tri-copter
            osd.setCursor(10, line++);
        } else {
            //escs 0, 2 and 4
            osd.setCursor(5, line);
        }
        printEscType(info.esc[i].type);
    }

    //Output Type, Receiver Type, LPF Frequency
    osd.setCursor(0, line++);
    osd.print(F("OUT:")); printOutputMode();
    osd.print(F(" RX:")); printRxType();
    osd.print(F(" LPF:")); printLpfFreq();

    //AUX_ARM, AUX_LEVEL, AUX_BUZZER, AUX_LED, AUX_3D
    osd.setCursor(2, line++);
    osd.print(F("ARM:")); printAuxConfig(AUX_ARM);
    osd.print(F(" LED:")); printAuxConfig(AUX_LED);
    osd.print(F(" 3D:")); printAuxConfig(AUX_3D);
    osd.setCursor(3, line++);
    osd.print(F("BUZZER:")); printAuxConfig(AUX_BUZZER);
    osd.print(F(" LEVEL:")); printAuxConfig(AUX_LEVEL);

    //Angle
    osd.setCursor(1, line++);
    osd.print(F("ROLL:")); printAngle(telemetry.angle[0], true);
    osd.print(F(" PITCH:"));  printAngle(telemetry.angle[1], true);

    //RX Channels
    osd.setCursor(0, line++);
    osd.print(F("T:"));  printMah(telemetry.throttle + 1000, false);
    osd.print(F(" R:")); printMah((telemetry.roll + 3000) / 2, false);
    osd.print(F(" P:")); printMah((telemetry.pitch + 3000) / 2, false);
    osd.print(F(" Y:")); printMah((telemetry.yaw + 3000) / 2, false);
    osd.setCursor(0, line++);
    osd.print(F("1:"));  printMah((telemetry.aux[0] + 3000) / 2, false);
    osd.print(F(" 2:")); printMah((telemetry.aux[1] + 3000) / 2, false);
    osd.print(F(" 3:")); printMah((telemetry.aux[2] + 3000) / 2, false);
    osd.print(F(" 4:")); printMah((telemetry.aux[3] + 3000) / 2, false);
}

static void updateInfo(){
    drawInfo();
    if(currentSticks == STICKS_NO || currentSticks == STICKS_LEFT){
        setCurrentView(VIEW_MAIN);
    }
}

/*
 * Flight Stats View
 * */

static void drawStats(){
    //show flight stats
    osd.setCursor(7,1);  osd.print(F("Flight Stats"));
    osd.setCursor(2,2);  osd.print(F(TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL));
    osd.setCursor(2,3);  osd.print(F("Flight Duration:  ")); printDuration(flight_stats.duration);
    osd.setCursor(2,4);  osd.print(F("Min Signal     :   ")); printPercentage(flight_stats.min_signal, true);
    osd.setCursor(2,5);  osd.print(F("Min Voltage    : ")); printVoltage(flight_stats.min_voltage, true);
    if(esc_stats.count){
        osd.setCursor(2,6);  osd.print(F("Max Current    :")); printCurent(flight_stats.max_current, true);
        osd.setCursor(2,7);  osd.print(F("Used mAh       :   ")); printMah(esc_stats.used_ah, false);
        osd.setCursor(2,8);  osd.print(F("Max RPMs       :  ")); printRpm(flight_stats.max_rpm, 14, false);
        osd.setCursor(2,9); osd.print(F("Max Temperature:   ")); printTemperature(flight_stats.max_temperature, true);
    }
}

static void updateStats(){
    if((currentSticks && currentSticks != STICKS_ERR) || (millis() - lastViewSwitch) > 10000){
        setCurrentView(VIEW_OFF);
    }
}

/*
 * Last Flight View
 * */

static void updateLastFlight(){
    if(currentSticks && currentSticks != STICKS_ERR){
        setCurrentView(VIEW_MAIN);
    }
}

/*
 * Main Menu View
 * */

static void drawMainMenu(){
    osd.setCursor(8,1); osd.print(F("Main Menu"));
    osd.setCursor(6,2); osd.print(F(TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL TB_HL));
    osd.setCursor(8,3); osd.print(F("Settings"));
    osd.setCursor(8,4); osd.print(F("PID Setup"));
    osd.setCursor(8,5); osd.print(F("RC Rates"));
    osd.setCursor(8,6); osd.print(F("Extra PIDs"));
    osd.setCursor(8,7); osd.print(F("Filters"));
    osd.setCursor(8,8); osd.print(F("Board Info"));
    osd.setCursor(8,9); osd.print(F("Last Stats"));
}

static void updateMainMenu(){
    if(currentSticks == STICKS_UP){
        if(!mainMenuIndex){
            mainMenuIndex = VIEW_LAST_FLIGHT;
        } else {
            mainMenuIndex -= 1;
        }
    } else if(currentSticks == STICKS_DOWN){
        mainMenuIndex += 1;
        if(mainMenuIndex > VIEW_LAST_FLIGHT){
            mainMenuIndex = 0;
        }
    }
    uint8_t i;
    for(i=0; i<(VIEW_MAIN); i++){
        osd.setCursor(4,3+i);
        osd.write((mainMenuIndex == i)?ARROW_RIGHT:' ');
        osd.write((mainMenuIndex == i)?ARROW_RIGHT:' ');
        osd.setCursor(19,3+i);
        osd.write((mainMenuIndex == i)?ARROW_LEFT:' ');
        osd.write((mainMenuIndex == i)?ARROW_LEFT:' ');
    }

    if(currentSticks == STICKS_RIGHT || currentSticks == STICKS_YES){
        setCurrentView((view_index_t)mainMenuIndex);
    } else if(currentSticks == STICKS_LEFT || currentSticks == STICKS_NO){
        setCurrentView(VIEW_OFF);
    }
}

/*
 * Telemetry Live Stats
 * Always visible on the screen
 * */

// Battery alarm shown only when armed
void voltageAlarm(int16_t voltage){
    static bool shown = false;
    if(!telemetry.armed || (uint16_t)voltage > (settings.vbat_alarm * 10)){
        if(shown){
            osd.clearLine(2);
            shown = false;
        }
        return;
    }
    if(shown){
        osd.clearLine(2);
    } else {
        osd.setCursor(7,2);
        osd.print(F("BATTERY LOW"));
    }
    shown = !shown;
}

// ESC Over Current alarm
void overCurrentAlarm(uint8_t index, bool active){
    if(!index){
        osd.setCursor(0,1);
    } else if(index == 1){
        osd.setCursor(22,1);
    } else if(index == 2){
        osd.setCursor(0,10);
    } else if(index == 3){
        osd.setCursor(22,10);
    } else {
        return;
    }
    if(active){
        osd.print(F("ALARM"));
    } else {
        osd.print(F("     "));
    }
}

//Always visible on the screen
void drawLiveStats(){
    video_mode_t mode = osd.detectVideoMode();
    static video_mode_t lastMode = mode;
    uint8_t bottomRow = 11;
    int16_t voltage = esc_stats.count?esc_stats.voltage:telemetry.voltage;
    uint8_t i;

    if(mode == VIDEO_NONE){
        mode = lastMode;
    }
    if(mode == VIDEO_PAL){
        bottomRow += 3;
    }

    if(lastMode != mode){
        lastMode = mode;
        if(mode == VIDEO_PAL){
            //clear the bottom status line from NTSC
            osd.clearLine(bottomRow);
        }
    }

    osd.setCursor(1,0);
    if(telemetry.armed){
        printDuration(millis() - flight_stats.duration);
    } else {
        printFlightMode();
    }
    osd.write(' '); printPercentage(100 - telemetry.failsafe, true);
    if(esc_stats.count){
        osd.write(' '); printTemperature(esc_stats.temperature, true); osd.write(' '); printRpm(esc_stats.rpm, 14, true);
    }

    osd.setCursor(2,bottomRow);
    printVoltage(voltage, true);
    if(esc_stats.count){
        osd.write(' '); printCurent(esc_stats.current, true); osd.write(' '); osd.write(' '); printMah(esc_stats.used_ah, true);
        if(telemetry.armed){
            for(i=0;i<esc_stats.count;i++){
                if(info.esc[i].type){
                    overCurrentAlarm(i, (telemetry.esc[i].current > getEscMaxCurrent(i)));
                }
            }
        } else {
            //try to detect the ESC types
            if(!info.count){
                update_info = true;
            } else {
                for(i=0;i<esc_stats.count;i++){
                    if(!info.esc[i].type){
                        update_info = true;
                    }
                }
            }
        }
    }
  
    if (telemetry.armed) {     
        // draw crosshair if armed and so configured
        drawCrosshair();
    }

    voltageAlarm(voltage);
}

/*
 * View Methods Table
 * */

static view_methods_t views[VIEW_BAD] = {
        {drawSettings, updateSettings, NULL},   //Settings
        {drawPids, updatePids, NULL},           //PIDs
        {drawRates, updateRates, NULL},         //Rates
        {drawXtraPids, updateXtraPids, NULL},   //Xtra PIDs
        {drawFilters, updateFilters, NULL},     //Filters
        {drawInfo, updateInfo, NULL},           //Info
        {drawStats, updateLastFlight, NULL},    //Last Flight
        {NULL, NULL, NULL},                     //Off
        {drawMainMenu, updateMainMenu, NULL},   //Main Menu
        {drawStats, updateStats, NULL}          //Flight Stats
};

/*
 * FC Event Logic
 * */

void loopDisarmed(){
    if((millis() - lastViewSwitch) < 1000){
        currentSticks = STICKS_ERR;
    }
    if(currentView == VIEW_OFF && currentSticks == STICKS_MENU){
        setCurrentView(VIEW_MAIN);
    }
    if(currentView != lastView){
        if(lastView != VIEW_OFF && views[lastView].deinit){
            views[lastView].deinit();
        }
        lastView = currentView;
        osd.clearDisplay();
        if(views[currentView].init){
            views[currentView].init();
        } else {
            setCurrentView(VIEW_OFF);
            lastView = VIEW_OFF;
        }
    }
    drawLiveStats();
    if(currentView != VIEW_OFF && views[currentView].update){
        views[currentView].update();
    }
}

void onDisarm(){
    stopStats();
    osd.clearDisplay();
    setCurrentView(VIEW_STATS);
    lastView = VIEW_OFF;
    update_info = true;
    update_settings = true;
}

void onArm(){
    startStats();
    osd.clearDisplay();
    setCurrentView(VIEW_OFF);
    lastView = VIEW_OFF;
}

void loopArmed(){
    drawLiveStats();
}

void loop(){
    static uint8_t itteration = 0;
    static uint8_t lastArmed = 0;

    if(kiss_update()){
        if(!telemetry.armed){
            currentSticks = (sticks_value_t)sticksGetValue();
        }
        if(lastArmed != telemetry.armed){
            lastArmed = telemetry.armed;
            if(!telemetry.armed){
                onDisarm();
            } else {
                onArm();
            }
            itteration = 9;
        }
        itteration++;
        if(telemetry.armed) {
            collectStats();
        }
        if(itteration > 9){
            itteration = 0;
            osd.setOffsetLeft(offsetLeft);
            osd.setOffsetTop(offsetTop);
            if(telemetry.armed || !hasArmSwitch()){
                loopArmed();
            } else {
                loopDisarmed();
            }
            osd.display();
        }
    } else {
        msp_delay(1000);
    }
}

void setup(){
    //kiss_set_dbg_cb(&onDbg);
    msp_on_packet(&onMSP);
    uart_init(115200);

    //clear telemetry and configuration
    memset(&telemetry, 0, sizeof(kiss_telemetry_t));
    memset(&settings, 0, sizeof(kiss_settings_t));

    //run some basic video detection
    osd.begin(VIDEO_NTSC);
    uint32_t startDetection = millis();
    while(osd.detectVideoMode() == VIDEO_NONE && (millis() - startDetection) < 1000){
        delay(10);
    }
    if(osd.getVideoMode() == VIDEO_PAL){
        osd.begin(VIDEO_PAL);
    }

    //setup OSD properties
    osd.setOffsetLeft(offsetLeft);
    osd.setOffsetTop(offsetTop);
    osd.setPrintMode(PRINT_BUFFERED);

    //show some status
    osd.clearDisplay();
    osd.setCursor(2,2);
    osd.println(F("Connecting to KISS FC..."));
    osd.display();

    //if FC is not available, listen for font upload
    while(!kiss_update()){
        msp_delay(1000);
    }

    //we are now connected to KISS FC
    osd.clearDisplay();
    osd.display();
}


static void drawCrosshair() {

#ifdef USE_CROSSHAIR
  osd.setCursor((osd.width()/2)-2, (osd.height()/2)-1);
  osd.println(F(CROSSHAIR));
#endif
}
/*
void onDbg(uint8_t cmd, uint8_t * data, uint8_t len){
    if(osd.getVideoMode() != VIDEO_PAL){
        return;
    }
    osd.setCursor(0,11);
    osd.print(F("DBG["));
    osd.print(len);
    osd.print(F("]:"));
    uint8_t i;
    for(i=0;i<len;i++){
        osd.write(' ');
        osd.print(data[i], HEX);
    }
    osd.display();
    delay(1000);
}
*/
