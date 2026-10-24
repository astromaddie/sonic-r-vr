/* Exercise the production opt-in recorder/configuration with a controlled CPU
 * clock. The discarded GL/XR functions are never run or replaced by a renderer. */
#include "vr/vr.c"
#include <assert.h>
static Uint64 fakeClock;static unsigned clockReads,seed;
Uint64 SDL_GetPerformanceCounter(void){clockReads++;return fakeClock;}
Uint64 SDL_GetPerformanceFrequency(void){return 1000;}
void Srand(unsigned int value){seed=value;}
int main(void){
 unsetenv("SONICR_VR_FIXTURE_TRACK");unsetenv("SONICR_VR_FIXTURE_WEATHER");unsetenv("SONICR_VR_FIXTURE_TIME");
 autoRace=1;g_raceType=RACE_TIMEATTACK;g_numPlayers=1;
 VR_ConfigureAutoRace();assert(g_raceType==RACE_TIMEATTACK && g_numPlayers==1);
 setenv("SONICR_VR_FIXTURE_TRACK","2",1);setenv("SONICR_VR_FIXTURE_WEATHER","2",1);setenv("SONICR_VR_FIXTURE_TIME","3",1);
 autoRace=0;VR_ConfigureAutoRace();assert(g_numPlayers==1); /* normal menus untouched */
 autoRace=1;VR_ConfigureAutoRace();assert(g_raceType==RACE_TYPE_GP && g_numPlayers==5);
 assert(g_trackId==2 && g_weatherConfig==WC_SNOW && g_timeOfDay==2 && seed==0x4BEE);
 for(int i=0;i<5;i++)assert(g_playerBase[i].charId==i);
 perfEnabled=0;assert(perfNow()==0);perfAdd(0,0);VR_PerfWeather(1);VR_PerfWeather(0);perfReport();
 RenderVertex v={0};modeRace=1;VR_TraceDraw(&v,1,NULL,0);VR_TraceArrayBatch(6);assert(!perfArraySubmits[0]);assert(clockReads==0 && perfDraws[0]==0 && perfSubmits[0]==0);
 perfEnabled=1;eye=-1;fakeClock=100;double start=perfNow();fakeClock=107;perfAdd(2,start);assert(perfMs[2]==7);
 VR_TraceDraw(&v,1,NULL,0);eye=0;VR_PerfWeather(1);VR_TraceDraw(&v,1,NULL,0);
 fakeClock=110;VR_PerfWeather(0);VR_TraceDraw(&v,1,NULL,0);VR_TraceBatch(6);eye=1;VR_TraceDraw(&v,1,NULL,0);VR_TraceArrayBatch(3);assert(perfArraySubmits[2]==1);
 assert(perfSubmits[1]==1 && perfSubmits[2]==1 && perfVertices[1]==6 && perfVertices[2]==3);
 assert(perfDraws[0]==1 && perfDraws[1]==2 && perfDraws[2]==1);
 assert(perfWeatherDraws[0]==0 && perfWeatherDraws[1]==1 && perfWeatherMs[1]==3);
 perfFrames=60;perfTicks=30;fakeClock=1000;perfReport();assert(!perfFrames && !perfMs[2] && !perfDraws[1] && !perfSubmits[1] && !perfVertices[2] && !perfArraySubmits[2]);
 puts("VR performance fixture: disabled clock/draw overhead gated; stage/eye/weather attribution and reset; explicit GP five-player weather/time/seed setup passed");
 return 0;
}
