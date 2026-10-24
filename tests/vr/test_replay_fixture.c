/* Actual VR parser/configuration/input/race lifecycle. Reuse the strict GL
 * boundary recorder for startup/shutdown; no GPU or owned assets required. */
#define main batch_fixture_main
#include "test_adjacent_batch.c"
#undef main
#include <sys/stat.h>
int g_precipVpRange[5];
void Srand(unsigned int value){assert(value==0x4BEE);}
static void word(FILE *f,unsigned v){assert(fputc(v&255,f)!=EOF);assert(fputc(v>>8,f)!=EOF);}
static FILE *demo(int count,int malformed){
 static const unsigned h[14]={1,5,1,1,0,0,1,0,1,0,1,2,3,4};
 FILE *f=tmpfile();assert(f);
 for(int i=0;i<14;i++)word(f,h[i]+(malformed==1 && i==0));
 word(f,count);
 for(int i=0;i<count;i++)word(f,i<60?0:PAD_UP|(i&1?PAD_LEFT:PAD_RIGHT)|PAD_START|4);
 if(malformed==2)fputc(0,f);
 if(malformed==3){assert(fflush(f)==0);assert(ftruncate(fileno(f),31)==0);}
 rewind(f);return f;
}
static void parserTests(const char *owned){
 for(int bad=0;bad<=3;bad++){
  FILE *f=demo(80,bad);assert(readReplay(f)==!bad);fclose(f);
  if(!bad){assert(replayCount==80 && replayIndex==0 && replayPads[60]==(PAD_UP|PAD_RIGHT|PAD_START|4));}
  else assert(!replayPads && !replayCount && !replayIndex);
 }
 FILE *f=demo(0,0);assert(!readReplay(f));fclose(f);
 f=tmpfile();assert(f);word(f,1);rewind(f);assert(!readReplay(f));fclose(f);
 if(owned && *owned){
  f=fopen(owned,"rb");assert(f && readReplay(f));fclose(f);assert(replayCount==1015);
  for(int i=0;i<60;i++)assert(replayPads[i]==0);
  unsigned gameplay=0;for(unsigned i=60;i<replayCount;i++)gameplay+=replayPads[i]!=0;
  assert(gameplay>0);printf("owned Island parser: 1015 LE pads, first60 neutral, %u subsequent active pads\n",gameplay);
 }
 resetReplay();assert(!replayPads && !replayCount && !replayIndex);
 puts("replay parser: 30-byte header, LE pad data, malformed header/truncation/trailing bytes/zero count rejected; private buffer reset passed");
}
static void gates(void){
 setenv("SONICR_VR_FIXTURE_REPLAY","1",1);setenv("SONICR_VR_AUTORACE","1",1);
 setenv("SONICR_VR_OFFHEAD_RENDER","1",1);setenv("SONICR_VR_RUN_SECONDS","30",1);
 unsetenv("SONICR_VR_DIAGNOSTIC");unsetenv("SONICR_VR_DEPTH");unsetenv("SONICR_VR_OFFHEAD_NEUTRAL");unsetenv("SONICR_VR_OFFHEAD_RUNTIME_FOV");
 assert(VR_Init() && fixtureReplay);VR_Shutdown();assert(!fixtureReplay && !replayPads);
 fflush(NULL);
 for(int bad=0;bad<4;bad++){
  pid_t p=fork();assert(p>=0);
  if(!p){
   if(bad==0)unsetenv("SONICR_VR_OFFHEAD_RENDER");
   if(bad==1)unsetenv("SONICR_VR_AUTORACE");
   if(bad==2)unsetenv("SONICR_VR_RUN_SECONDS");
   if(bad==3)setenv("SONICR_VR_DIAGNOSTIC","1",1);
   VR_Init();_exit(0);
  }
  int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status) && WEXITSTATUS(status)!=0);
 }
 fixtureReplay=1;autoRace=1;unsetenv("SONICR_VR_FIXTURE_TRACK");
 setenv("SONICR_VR_FIXTURE_WEATHER","2",1);unsetenv("SONICR_VR_FIXTURE_TIME");
 g_numPlayers=1;g_raceType=RACE_TIMEATTACK;VR_ConfigureAutoRace();
 assert(g_trackId==TRACK_RESORT_ISLAND && g_numPlayers==5 && g_raceType==RACE_TYPE_GP && g_weatherConfig==WC_SNOW);
 fflush(NULL);pid_t p=fork();assert(p>=0);
 if(!p){setenv("SONICR_VR_FIXTURE_TRACK","2",1);VR_ConfigureAutoRace();_exit(0);}
 int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status) && WEXITSTATUS(status)!=0);
 puts("replay production gates: bounded runtime offhead+autorace only; fixed Island GP5, weather configurable; other track rejected");
}
static void lifecycle(const char *directory){
 assert(chdir(directory)==0);assert(mkdir("BIN",0700)==0);assert(mkdir("BIN/DEMOS",0700)==0);
 FILE *src=demo(80,0),*dst=fopen("BIN/DEMOS/ISLAND.DEM","wb");assert(dst);
 int ch;while((ch=fgetc(src))!=EOF)assert(fputc(ch,dst)!=EOF);fclose(src);assert(fclose(dst)==0);
 setup(1,0);fixtureReplay=autoRace=1;g_demoMode=DEMO_NONE;
 g_introCountdown=210;g_isPaused=g_postRaceCameraMode=0;
 VR_RaceStart();assert(g_introCountdown==60 && replayIndex==0 && replayCount==80 && g_demoMode==DEMO_NONE);
 for(int i=60;i>0;i--){
  assert(g_introCountdown==i);g_perPlayerInput[0]=0xffff;g_inputBits=12;
  VR_DiagnosticInput();assert(g_perPlayerInput[0]==0 && !g_inputBits && replayIndex==(unsigned)(61-i));
  g_introCountdown--; /* Native UpdateLapCounter follows the input/physics tick. */
 }
 memset(g_taGhostSource,0,80*sizeof *g_taGhostSource);
 VR_DiagnosticInput();assert(replayIndex==61 && g_perPlayerInput[0]==(PAD_UP|PAD_RIGHT));
 assert(g_combinedInputState==g_perPlayerInput[0] && g_demoMode==DEMO_NONE);
 g_isPaused=1;VR_DiagnosticInput();assert(replayIndex==61 && !g_perPlayerInput[0]);g_isPaused=0;
 g_postRaceCameraMode=1;VR_DiagnosticInput();assert(replayIndex==61 && !g_perPlayerInput[0]);g_postRaceCameraMode=0;
 VR_DiagnosticInput();assert(replayIndex==62 && g_perPlayerInput[0]==(PAD_UP|PAD_LEFT));
 while(replayIndex<replayCount)VR_DiagnosticInput();
 VR_DiagnosticInput();assert(replayIndex==80 && !g_perPlayerInput[0]);
 VR_RaceStart();assert(replayIndex==0 && replayCount==80 && g_introCountdown==60);
 g_introCountdown=61;VR_DiagnosticInput();assert(!replayIndex && !g_perPlayerInput[0]);
 VR_Shutdown();assert(!fixtureReplay && !replayPads && !replayCount && !replayIndex);
 diagnostic=drive=0;g_perPlayerInput[0]=PAD_START;g_inputBits=12;g_combinedInputState=PAD_START;
 VR_DiagnosticInput();assert(g_perPlayerInput[0]==PAD_START && g_inputBits==12 && g_combinedInputState==PAD_START);
 puts("replay input lifecycle: 60 native countdown inputs, gameplay-only pads, private data survives native recording, pause/postrace hold, exhaustion neutral, race/shutdown reset, normal input unchanged");
}
int main(int argc,char **argv){assert(argc==3);parserTests(argv[2]);gates();lifecycle(argv[1]);return 0;}
