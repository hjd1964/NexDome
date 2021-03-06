/*
 *  This package includes the drivers and sources for the NexDome
 *  Copyright (C) 2016 Rozeware Development Ltd.
 *
 *  NexDome is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NexDome is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NexDome files.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *   This file contains:-
 *   Firmware for the arduino leonardo in the NexDome rotation controller
 *   Hardware is the leonardo clone, with an xbee series 1 module
 *   and a TB6600 based stepper motor driver
 */

//  Libraries we need to include
#include <AccelStepper.h>
#include <EEPROM.h>

#define VERSION_MAJOR 1
#define VERSION_MINOR 10

/*
 *   Arduino pins
 *   
 *   0 - Serial
 *   1 - Serial
 *   2 - Irq Sensor (home for rotator, open for shutter)
 *   3 - Irq Sensor (unused rotator, closed shutter)
 *   4 -
 *   5 - Button
 *   6 - Button
 *   7 -
 *   8 - Reserved for AltSoftSerial if not on leonardo
 *   9 - Soft serial for AltSoftSerial if not on Leonardo
 *   10- Motor Enable
 *   11- Motor Dir
 *   12- Motor Step
 *   13- On board led, we dont use it
 *   
 *   These 3 used only when the small prototype hacked for the big easy driver
 *   is in use for bench testing
 *   A0- MS1
 *   A1- MS2
 *   A2- MS3
 * 
 */

//  Pin definitions
//  on the uno, ie 328 based units, only pins 2 and 3 are available for interrupts
//  for the rotator, home sensor will be on pin2 and for the shutter, both pins used for limit switches
//  home sensor is on pin 2, we use interrupts here to detect home sensor
#define HOME 2
//  We dont use the irq on pin 3 for the dome rotator
//  but we do use it for the shutter switches
#define OTHER_IRQ 3

/*
 *   Defining which pins for serial gets tricky
 *   on the leonardo, no big deal, we just use the standard hardware serial port on pins 0 and 1
 *   but for uno etc, if we intelligently choose our serial pins, we can use alternate better
 *   performing libraries
 *   
 *   SoftSerial can go on any pins
 *   AltSoftSerial can only go on tx 9 rx 8 on the uno (or other atmega328) boards
 *   There is also 'yetanothersoftserial', but it uses the two interrupt pins, 2 and 3
 *   which we want for our sensors
 *   
 */

// HJD OCS ---> unused
#define SERIAL_TX 9
#define SERIAL_RX 8

//  HJD OCS --->  Default pins here, none are used for my observatory control system
//  Enable line for the stepper driver
#define EN 10
//  Direction line for the stepper driver
#define DIR 11
//  Step line for the stepper driver
#define STP 12
//  Our two buttons for manual movement
#define BUTTON_EAST 5
#define BUTTON_WEST 6

//  Physical definitions of the gears
//  This is the gearbox on the motor
//float ReductionGear=(float)15+((float)3/(float)10);
#define REDUCTION_GEAR 15.3
#define DOME_TEETH 288
#define GEAR_TEETH 15
#define DOME_TURN_TIME 90
// set this to match the type of steps configured on the
// stepper controller
#define STEP_TYPE 8

//  For reading our input voltage 
//  HJD OCS --->  default A0 is used on my OCS, choose another pin like A8
#define VPIN A8

//  if we define our serial ports this way, then re-compiling for different port assignments
//  on different processors, becomes easy and we dont have to hunt all over chaning
//  code to reference different serial definitions
//  HJD OCS --->  for Mega2560 choose Serial1 and Serial2 since they're not used by my observatory control system
#define Computer Serial1
#define Wireless Serial2
#define Debug Serial1

#define SHUTTER_STATE_NOT_CONNECTED 0
#define SHUTTER_STATE_OPEN 1
#define SHUTTER_STATE_OPENING 2
#define SHUTTER_STATE_CLOSED 3
#define SHUTTER_STATE_CLOSING 4
#define SHUTTER_STATE_UNKNOWN 5
#define SHUTTER_VERSION_LENGTH 5

//  The shutter sleep period ss 15 seconds
//  Lets query it on every second sleep cycle
//  but send one second early to ensure we catch it
//  when it wakes up
#define SHUTTER_SLEEP_WAIT 29000
//  When in alive mode, the timing for each cycle is one second
//  Query slightly early so the query is waiting when it wakes up
#define SHUTTER_AWAKE_WAIT 900

//  Globals needed
//  this one is set by the home sensor interrupt routines
//  needs to be volatile because of access during interrupts
volatile bool HomeSensor=false;
volatile bool SenseRising=false;
float HeadingError=0;
float LastHeadingError=0;
//long int LastWirelessCheck;
int ShutterState=SHUTTER_STATE_NOT_CONNECTED;
float ShutterPosition=0;
unsigned int ShutterQueryTime;
char ShutterVersion[SHUTTER_VERSION_LENGTH];
unsigned long int ShutterHibernateTimer=0;
unsigned long int LastCommandTime=0;

int BatteryVolts=0;
int ShutterBatteryVolts=0;
int LowVoltCutoff=0;
bool DomeIsReversed=false;

//  The dome class will use an accel stepper object to run the motor
AccelStepper accelStepper(AccelStepper::DRIVER, STP, DIR);

//  HJD OCS ---> doesn't touch locations from 4 to 99 so this is ok
#define EEPROM_LOCATION 10
#define SIGNATURE 1036
typedef struct DomeConfiguration {
  int signature;
  long int StepsPerDomeTurn;
  float HomeAzimuth;
  float ParkAzimuth;
  bool IsReversed;
  float Checksum;
} dome_config;

//  Functions that need definitions
void ConfigureWireless();
void HomeInterrupt();

//  Now declare the class for our dome
class NexDome
{
  private:
    long int TargetSteps;
    long int LastHomeCount;
  public:
    NexDome();
    ~NexDome(){}; //  empty destructor because it'll never get used

    //  exposed variables
    bool Active;  //  if the motor is in motion this is true
                  //  exposed publicly because it's performance sensative to reference
    bool FindingHome;
    bool Calibrating;
    bool isAtHome();
    bool HasBeenHome;
    
      
    float HomeAzimuth;
    float ParkAzimuth;
    long int StepsPerDomeTurn;
    //  public functions
    int SetStepsPerDomeTurn(long int);
    void MoveTo(long int);
    long int CurrentPosition();
    void Stop();
    bool Run();
    void EnableMotor();
    void DisableMotor();
    float GetHeading();
    int SetHeading(float);
    int AtHome();   //  called by interrupt routines when home sensor is detected
    void FindHome();
    void Calibrate();
    int Sync(float);
    //int SetHomeAzimuth(float);
    //float GetHomeAzimuth();
    long int AzimuthToTicks(int);
    bool SaveConfig();
    bool ReadConfig();

    int WriteCalibrationToEEprom();
    int ReadCalibrationFromEEprom();
};

// Every class needs a constructor
NexDome::NexDome()
{
  Active=false; //  we are not in motion when we start up
  TargetSteps=0;
  LastHomeCount=0;
  FindingHome=false;
  //SettingHome=false;
  Calibrating=false;
  HomeAzimuth=0;  //  an uncalibrated dome
  ParkAzimuth=180;
  HasBeenHome=false;
  accelStepper.setPinsInverted(true,false,false);
  memset(ShutterVersion,0,SHUTTER_VERSION_LENGTH);
}

bool NexDome::SaveConfig()
{
  dome_config cfg;

  //  zero the structure so currently unused parts
  //  dont end up loaded with random garbage
  memset(&cfg,0,sizeof(cfg));

  cfg.signature=SIGNATURE;
  cfg.StepsPerDomeTurn=StepsPerDomeTurn;
  cfg.HomeAzimuth=HomeAzimuth;
  cfg.ParkAzimuth=ParkAzimuth;
  cfg.IsReversed=DomeIsReversed;
  
  EEPROM.put(EEPROM_LOCATION,cfg);
  return true;
}

bool NexDome::ReadConfig()
{
  dome_config cfg;

  //  zero the structure so currently unused parts
  //  dont end up loaded with random garbage
  memset(&cfg,0,sizeof(cfg));
  EEPROM.get(EEPROM_LOCATION,cfg);
  /*
  unsigned char *pt;
  int value;
  pt=(unsigned char *)&cfg;
  for(int x=0; x<sizeof(dome_config); x++) {
    value=EEPROM.read(EEPROM_LOCATION+x);
    pt[x]=(unsigned char) value;
  }
  */
  if(cfg.signature != SIGNATURE) {
    //Debug.write("Invalid eeprom data");
    return false;
  }
  SetStepsPerDomeTurn(cfg.StepsPerDomeTurn);
  HomeAzimuth=cfg.HomeAzimuth;
  ParkAzimuth=cfg.ParkAzimuth;
  DomeIsReversed=cfg.IsReversed;

  if(DomeIsReversed) accelStepper.setPinsInverted(false,false,false);

  //Debug.print("Steps per turn from eeprom is ");
  //Debug.print(cfg.StepsPerDomeTurn);
  //Debug.println("");
  //Debug.println(HomeAzimuth);
  //Debug.println(ParkAzimuth);
  //Debug.print("Reversed ");
  //Debug.println(DomeIsReversed);
  return true;
}

int NexDome::SetStepsPerDomeTurn(long int steps)
{
  long int StepsPerSecond;

  //Debug.println("Setting steps per turn to ");
  //Debug.println(steps);
  
  StepsPerDomeTurn=steps;
  StepsPerSecond=StepsPerDomeTurn/DOME_TURN_TIME;
  if(StepsPerSecond < 2000) StepsPerSecond=2000;
  //Debug.print(StepsPerSecond);
  //Debug.println(" Steps per second");
  accelStepper.setMaxSpeed(StepsPerSecond);
  //  Set a ramp time
  accelStepper.setAcceleration(StepsPerSecond*2);
  
  return 0;
}

void NexDome::EnableMotor()
{
  //Debug.print("Enable at ");
  //Debug.println(CurrentPosition());
  digitalWrite(EN,LOW);
  delay(100);
  Active=true;  
}

void NexDome::DisableMotor()
{
  digitalWrite(EN,HIGH);

  //digitalWrite(DIR,LOW);
  //digitalWrite(STP,LOW);
  
  Active=false;
  //Debug.println("Stopped");
}

bool NexDome::Run()
{
  bool r;
  long int pos;
  r=accelStepper.run();
  if(!r) {
    //  This move has finished
    DisableMotor();
    //  Motor is stopped, so now is the time to handle wrap arounds
    pos=CurrentPosition();
    //if(pos==LastHomeCount) isAtHome=true;
    if(pos < 0) {
      while(pos < 0) pos=pos+StepsPerDomeTurn;
      accelStepper.setCurrentPosition(pos);
    }
    if(pos >= StepsPerDomeTurn) {
      while(pos >= StepsPerDomeTurn) pos=pos-StepsPerDomeTurn;
      accelStepper.setCurrentPosition(pos);
    }
  }
  return r;
}

long int NexDome::CurrentPosition()
{
  return accelStepper.currentPosition();
}

//  for the stop function, we want to abandon a target
//  and just start the deceleration, but we want it to stop
//  on an even step boundary, so we do some math hoops to
//  ensure our stop point is on a whole step, not a microstep
void NexDome::Stop()
{
  long int current;
  //long int nowstep;
  int r;

  FindingHome=false;
  Calibrating=false;
  if(!Active) return;   //  we are already stopped

  //  round to even steps
  current=accelStepper.currentPosition();
  r=current%STEP_TYPE;
  current=current-r;
  accelStepper.moveTo(current);

  return;
}

//  this is a raw move to a stepper count
void NexDome::MoveTo(long int p)
{
  long int current;
  current=accelStepper.currentPosition();
  //Debug.println(current);
  //Debug.println(p);
  if(p > current) {
    //Debug.println("Set sense rising");
    SenseRising=true;
  } else {
    //Debug.println("Set sense falling");
    SenseRising=false;
  }

  if(!Active) EnableMotor();
  accelStepper.moveTo(p);
  //isAtHome=false;
  TargetSteps=p;
  return;
}

long int NexDome::AzimuthToTicks(int az)
{
  long int ticks;
  int r;
  ticks=(float)StepsPerDomeTurn/(float)360*az;
  //  now round to whole steps, dropping microsteps
  r=ticks%STEP_TYPE;
  ticks=ticks-r;
  return ticks;
}

float NexDome::GetHeading()
{
  /* floating point math for get heading  
   *  prefer floats for accurately placing
   *  the power pickup to charge the shutter
   *  but it introduces significant processing
   *  overhead
   */
  
  float Heading;
  //Debug.print("Current ");
  //Debug.println(CurrentPosition());
  Heading=(float)CurrentPosition()/(float)StepsPerDomeTurn*360;
  //  in case we need to do another step
  //  do a run sequence now
  if(Active) Run();
  while(Heading < 0) Heading+=360;
  while(Heading >= 360) Heading-=360;
  if(Active) Run();
  return Heading;
  
/*
  long int h;

  //Debug.println(StepsPerDomeTurn);
  h=CurrentPosition();
  //Debug.println(h);
  h=h*360+StepsPerDomeTurn/2;
  //  fix for rounding issues
  //h=h+StepsPerDomeTurn/2;
  h=h/StepsPerDomeTurn;
  //Debug.println(h);
  while(h < 0) h+=360;
  while(h >= 360) h-=360;
  return (int) h;
*/

}

int NexDome::SetHeading(float h)
{
  float target=0;
  //long int TargetSteps;
  float current;
  int r;
  float delta;
  float newdelta;
  //bool GoingBackwards;
  //long int StepsToGo;

  FindingHome=false;
  //SettingHome=false;
  Calibrating=false;
  current=GetHeading();
  delta=h-current;
  if(delta == 0) return 0; //  we are already there
  
  if((delta <= 180)&&(delta >= -180)) {
    target=current+delta;
  }
  if(delta > 180) {
    //  its more than 180 degrees ahead
    newdelta=360-delta;
    target=current-newdelta;
  }
  if(delta < -180) {
    newdelta=360+delta;
    target=current+newdelta;
  }

/*
  Debug.print("heading now ");
  Debug.println(current);
  Debug.print("desired heading ");
  Debug.println(h);
  Debug.print("delta is ");
  Debug.println(delta);
  Debug.print("new target is ");
  Debug.println(target);
*/

  /*
  target=h/360.0*StepsPerDomeTurn;
  */
  TargetSteps=target/360*StepsPerDomeTurn;
  //Debug.println(CurrentPosition());
  //Debug.println(TargetSteps);

  //  now round to even steps, not microsteps
  r=TargetSteps%STEP_TYPE;
  TargetSteps=TargetSteps-r;
  
  MoveTo(TargetSteps);
  return 0;
}

int NexDome::Sync(float val)
{
  long int newaz;

  newaz=AzimuthToTicks(val);
  //Debug.print("Setting new tick count ");
  //Debug.print(newaz);
  accelStepper.setCurrentPosition(newaz);
  //Debug.print("sync with ");
  //Debug.println(val);
  return 0;
}

int NexDome::AtHome()
{
  int r;

  HasBeenHome=true;
  LastHomeCount=CurrentPosition();
  r=LastHomeCount%STEP_TYPE;
  LastHomeCount=LastHomeCount-r;
  //Debug.print("Home sensor detected at ");
  //Debug.print(LastHomeCount);
  //Debug.print(" ");
  //Debug.println(GetHeading());
  if(FindingHome) {
    if(Calibrating) {
      //  we often get a second hit on the other edge
      //  when we were stopped within the magnet range
      //  if calibrating, ignore a hit that comes to quickly
      if(LastHomeCount < 5000) {
        //Debug.println("Ignore spurios home hit while calibrating");
        return 0;
      }
      //Debug.print("T ");
      //Debug.println(LastHomeCount);
      //StepsPerDomeTurn=LastHomeCount;
      SetStepsPerDomeTurn(LastHomeCount);
      SaveConfig(); //  store this new value into the eeprom
      Calibrating=false;
    }
    FindingHome=false;
    //accelStepper.setCurrentPosition(0);
    MoveTo(LastHomeCount);
    //SettingHome=true;
  }
  return 0;
}

void NexDome::FindHome()
{
  //  we dont have to find home if we are already there
  if(isAtHome()) return;
  SetHeading(HomeAzimuth);
  FindingHome=true;
}

void NexDome::Calibrate()
{
  HomeAzimuth=0;
  accelStepper.setCurrentPosition(0);
  MoveTo(CurrentPosition() + 5000);
  Calibrating=true;
  FindingHome=true;
}

bool NexDome::isAtHome()
{
  int a;

  a=digitalRead(HOME);
  if(a==0) return true;
  return false;
}

void HomeInterrupt()
{
  //  lets debounce this just a little
  int a=digitalRead(HOME);
  if(SenseRising) {
    if(a==1) HomeSensor=true;
  } else {
    if(a==0) HomeSensor=true;
  }
  //if(a==0) {
    //Debug.println("Home Sensor");
    //Debug.println(a);
    //Debug.println("Home");
    //HomeSensor=true;
  //}
}

/*
void OtherInterrupt()
{
  //  lets debounce this just a little
  int a,b;
  a=digitalRead(HOME);
  b=digitalRead(HOME);
  if(a==b) {
    //Debug.print("Home Sensor");
    //Debug.println(a);
    Computer.println("Other");
    HomeSensor=true;
  }
}
*/

//  We have defined the class, now lets create one
NexDome Dome;

#ifdef ONCUE_OCS
void NexDomeSetup() {
#else
void setup() {
#endif
  float MotorTurnsPerDomeTurn;
  float StepsPerGearTurn;
  long int StepsPerDomeTurn;
  //float StepsPerSecond;
  long int StepsPerDegree;

  // We need the internal pullups on our input pins
  pinMode(HOME,INPUT_PULLUP);
  //  We dont need this on rotation controller
  //  but we use the same boards for testing so
  //  set the mode in case there is a switch connected
  //  because we are using a shutter board to test
  //  rotation code or vice versa
  pinMode(OTHER_IRQ,INPUT_PULLUP);
  pinMode(BUTTON_EAST,INPUT_PULLUP);
  pinMode(BUTTON_WEST,INPUT_PULLUP);

  pinMode(VPIN,INPUT);

  pinMode(STP, OUTPUT);
  pinMode(DIR, OUTPUT);
  pinMode(EN, OUTPUT);

  //  lets set up an interrupt from our home sensor
  attachInterrupt(digitalPinToInterrupt(HOME),HomeInterrupt,CHANGE);
  
  Computer.begin(9600);
  Wireless.begin(9600);
  //Debug.begin(9600);

  //  hack for development, we want it to wait now for the serial port to be connected
  //  so we see the output in the monitor during init
  //while(!Serial) {
  //}
  //Debug.println("Starting NexDome"); 

  MotorTurnsPerDomeTurn = (float)DOME_TEETH / (float) GEAR_TEETH;
  StepsPerGearTurn=200.0*(float)REDUCTION_GEAR*(float)STEP_TYPE;
  StepsPerDomeTurn=MotorTurnsPerDomeTurn*StepsPerGearTurn;
  StepsPerDegree=StepsPerDomeTurn/360;

  //Debug.print(StepsPerDomeTurn);
  //Debug.println(" Calculated steps per turn");
  
  Dome.EnableMotor();
  Dome.DisableMotor();
  Dome.SetStepsPerDomeTurn(StepsPerDomeTurn);

  Dome.ReadConfig();

  //Debug.print(Dome.StepsPerDomeTurn);
  //Debug.println(" steps per dome turn");
  //Debug.print(StepsPerDegree);
  //Debug.println(" steps per degree");
  
  ShutterQueryTime=SHUTTER_SLEEP_WAIT;
  ConfigureWireless();

  if(Dome.isAtHome()) {
    //  the dome is at the home sensor now
    if(Dome.HomeAzimuth != 0) Dome.Sync(Dome.HomeAzimuth);
  }
}

//  We dont need to check the buttons
//  on every loop, it adds a lot of overhead
//  routing thru digitalRead functions
//  We only need to do it 10 times a second max
unsigned long int LastButton=0;
int CheckButtons()
{
  unsigned long int now;

  //  time now
  now=millis();
  if(now < LastButton) {
    //  the millisecond timer has rolled over
    LastButton=now;
  }
  if(now-LastButton < 100) {
    //  return if it's less than our time between checks
    return -1;
  }
  //Serial.println(now-LastButton);
  LastButton=now;
 
  int bw,be;
  int buttonstate;
  //Debug.println(accelStepper.currentPosition());
  //  First we check buttons for any button state
  bw=digitalRead(BUTTON_WEST);
  be=digitalRead(BUTTON_EAST);
  buttonstate=bw+(be<<1);
  buttonstate=buttonstate ^ 0x03;
  //Debug.println(buttonstate);
  return buttonstate;
}

// define a buffer and pointers for our serial data state machine
// this definition conflicts with arduino headers in later versions
// we can just run with what they have defined for now
#define MY_SERIAL_BUFFER_SIZE 20

char SerialBuffer[MY_SERIAL_BUFFER_SIZE];
int SerialPointer=0;
bool SerialTarget=true;

//  Now define a buffer and pointers for our serial link over wireless
//  for talking to the shutter
char WirelessBuffer[MY_SERIAL_BUFFER_SIZE];
int WirelessPointer=0;

//  and some variables used to time out the state machines
unsigned long int LastShutterResponse=0;
unsigned long int LastShutterKeepAlive=0;
bool ShutterAlive=false;
bool DoingWirelessConfig=false;
int WirelessConfigState=0;
bool FoundXBee=false;

/*
 * When processing time sensative serial commands
 * we use sprintf and write to avoid Serial library overhead
 * which causes the stepper to stutter during command
 * exchanges if using simple print and println functions
 * 
 * On arduino, sprintf doesn't support floating point
 * so for float returns we format with dtostrf 
 * 
 * We process only a subset of commands when the dome is
 * in motion, because some of the shutter commands can tak
 * a few seconds to complete, and we dont want to make our
 * state machines so complex they dont fit in arduino memory
 * 
 * The simple solution, only process dome status commands while
 * in motion, and process the rest when dome is not running
 * 
 */
void ProcessSerialCommand()
{
  char buf[20];

  //  reset our drop dead timer
  LastCommandTime=millis();
  
  /* Abort current motion */
  if(SerialBuffer[0]=='a') {
    //SerialTarget=false;
    Dome.Stop();
    //  abort any shutter movement too
    Wireless.println("a");
    Computer.println("A");
  }
  /*  Raw position count */
  if(SerialBuffer[0]=='p') {
    sprintf(buf,"P %ld\n",Dome.CurrentPosition());
    Computer.write(buf,strlen(buf));
    SerialPointer=0;
    return;
  }
  /* is dome in motion */
  if(SerialBuffer[0]=='m') {
    int state=0;
    //Debug.write("M ");
    //  if dome is in motion, figure out what kind of motion
    if(Dome.Active) {
      Dome.Run();
      //  default is just a normal move
      state=1;
      if(Dome.FindingHome) state=2;
      if(Dome.Calibrating) state=3;
    }
    sprintf(buf,"M %d\n",state);
    Computer.write(buf);
    //Debug.write("\n");
    //Debug.println(state);
    SerialPointer=0;
    return;

  }
  /* query current heading */
  if(SerialBuffer[0]=='q') {
    //Debug.print("H ");
    //Debug.println(Dome.GetHeading());
    dtostrf(Dome.GetHeading(),2,1,buf);
    if(Dome.Active) Dome.Run();
    ////sprintf(buf,"H %s\n",b);
    Computer.write("Q ");
    Computer.write(buf);
    Computer.write("\n");
    //Debug.print("Q ");
    //Debug.println(Dome.GetHeading());
    SerialPointer=0;
    return;
  }

  /*
   * These commands should not be processed when the dome is in motion
   * 
   */

   /* query shutter state */
   if(SerialBuffer[0]=='u') {
    if(Dome.Active) Dome.Run();
    Computer.print("U ");
    Computer.println(ShutterState);
   }
   /*  get shutter position  */
   if(SerialBuffer[0]=='b') {
    if(Dome.Active) Dome.Run();
    Computer.print("B ");
    Computer.println(ShutterPosition);
   }
   /*  Open Shutter  */
   if(SerialBuffer[0]=='d') {
     Computer.println("D");
     //  tell the shutter to open
     Wireless.println("o");
     //  set shutter state to opening, and it'll get set to whatever the shutter
     //  is really doing on the next shutter status report
     if(ShutterState != SHUTTER_STATE_NOT_CONNECTED) ShutterState=SHUTTER_STATE_OPENING;
     //  now query status
     //delay(300);
     //Wireless.println("s");
     //ShutterQueryTime=SHUTTER_AWAKE_WAIT;
   }
   /*  Close Shutter  */
   if(SerialBuffer[0]=='e') {
     Computer.println("D");
     Wireless.println("c");
     //  set the state to closing
     //  and the next time shutter responds, it'll get set to shutter condition again
     if(ShutterState != SHUTTER_STATE_NOT_CONNECTED) ShutterState=SHUTTER_STATE_CLOSING;
     SerialTarget=true;
   }
    /*  Set Shutter Position */
   if(SerialBuffer[0]=='f') {
     float tt;
    
     Computer.println("F");
     tt=atof(&SerialBuffer[1]);
     Wireless.print("f ");
     Wireless.println(tt);
     //delay(300);
     //Wireless.println("s");
     //ShutterQueryTime=SHUTTER_AWAKE_WAIT;
   }
 
  // Wake up the shutter so it's more responsive while config
  //  dialog is on screen
  if(SerialBuffer[0]=='x') {
    Wireless.println("x");
    Computer.println("X");
    //ShutterQueryTime=SHUTTER_AWAKE_WAIT;
  }
  /* sync on this heading */
  if(SerialBuffer[0]=='s') {
    float h;
    float hnow;
    float delta;
    //float oldhome;
    float newhome;
    //  dont adjust home sensor if we have not seen it to
    //  verify where it is, this prevents a sync on park position
    //  from screwing it up when we power up
    h=atof(&SerialBuffer[1]);
    if(Dome.HasBeenHome) {
      //  We are syncing, lets calculate the new offset for the home
      //  sensor
      hnow=Dome.GetHeading();
      delta=hnow-h;
      newhome=Dome.HomeAzimuth-delta;
      if(newhome < 0) newhome=newhome+360;
      if(newhome > 360) newhome=newhome-360;
      if(newhome==360) newhome=0;
      Dome.HomeAzimuth=newhome;
      Dome.SaveConfig();
    } else {
      //Debug.println("Dome has not been home");
    }
    if((h >=0)&&(h<360)) {
       Computer.print("S ");
       Computer.println(h);
       Dome.Sync(h);
    } else {
      Computer.println("E ");
    }
  }
  if(SerialBuffer[0]=='o') {
    Computer.print("O ");
    Computer.println(LastHeadingError);
    //  Now set this to the last one we caught
    //  So we will report the next one, even if it's tiny
    LastHeadingError=HeadingError;
  }
  /* current status of steps per turn */
  if(SerialBuffer[0]=='t') {
    Computer.print("T ");
    Computer.println(Dome.StepsPerDomeTurn);
    //Debug.println(" Steps per Dome turn");
    //Debug.println(Dome.HomeAzimuth);
    //Debug.println(" Home Azimuth");
    
  }
  /* query for home azimuth */
  if(SerialBuffer[0]=='i') {
    Computer.print("I ");
    Computer.println(Dome.HomeAzimuth);
    //Debug.println(" Steps per Dome turn");
    //Debug.println(Dome.HomeAzimuth);
    //Debug.println(" Home Azimuth");
    
  }
  /* Set the home azimuth */
  if(SerialBuffer[0]=='j') {
    //  dont set this if we got an empty set
    if((SerialBuffer[1]!=0x0a)&&(SerialBuffer[1]!=0x0d)) {
      float newaz;
      newaz=atof(&SerialBuffer[1]);
      if((newaz >=0)&&(newaz < 360)) Dome.HomeAzimuth=newaz;
    }
    //  Respond same as a query for home azimuth
    Computer.print("I ");
    Computer.println(Dome.HomeAzimuth);
    Dome.SaveConfig();
    //Debug.println(" Steps per Dome turn");
    //Debug.println(Dome.HomeAzimuth);
    //Debug.println(" Home Azimuth");
    
  }
  /* query for park azimuth */
  if(SerialBuffer[0]=='n') {
    Computer.print("N ");
    Computer.println(Dome.ParkAzimuth);
    //Debug.println(" Steps per Dome turn");
    //Debug.println(Dome.HomeAzimuth);
    //Debug.println(" Home Azimuth");
    
  }
  /* Set the park azimuth */
  if(SerialBuffer[0]=='l') {
    float newaz;
    newaz=atof(&SerialBuffer[1]);
    if((newaz >=0)&&(newaz < 360)) Dome.ParkAzimuth=newaz;
    //  Respond same as a query for park
    Computer.print("N ");
    Computer.println(Dome.ParkAzimuth);
    Dome.SaveConfig();
    //Debug.println(" Steps per Dome turn");
    //Debug.println(Dome.HomeAzimuth);
    //Debug.println(" Home Azimuth");
  }
  
/*  goto based on a heading */
  if(SerialBuffer[0]=='g') {
    float target;
    target=atof(&SerialBuffer[1]);
    if((target >=0.0)&&(target <= 360.0)) {
      SerialTarget=true;
      Dome.SetHeading(target);
      Computer.println("G");
    } else {
      Computer.println("E");
    }
  }
  /* find the home position */
  if(SerialBuffer[0]=='h') {
    Dome.FindHome();
    SerialTarget=true;
    Computer.println("H");
  }
  /* are we at the home position */
  if(SerialBuffer[0]=='z') {
    Computer.print("Z ");
    if(Dome.isAtHome()) Computer.println("1");
    else {
      if(Dome.HasBeenHome) {
        Computer.println("0");
      }else {
        Computer.println("-1");
      }
    }
  }
  /* query for battery info */
  if(SerialBuffer[0]=='k') {
    if(SerialBuffer[1]==' ') {
      int newcutoff;
      newcutoff=atoi(&SerialBuffer[1]);
      Wireless.print("b ");
      Wireless.println(newcutoff);
    }
    Computer.print("K ");
    Computer.print(BatteryVolts);
    Computer.print(" ");
    Computer.print(ShutterBatteryVolts);
    Computer.print(" ");
    Computer.println(LowVoltCutoff);
  }
  /* calibrate the number of steps in a full turn */
  if(SerialBuffer[0]=='c') {
    //  calibrate the dome
    //  In order to calibrate, we need to be at the home position
    if(Dome.isAtHome()) {
      Computer.println("C");
      SerialTarget=true;
      Dome.Calibrate();
      
    } else {
      Computer.println("E");
    }
  }
  //  get / set the reversed flag
  if(SerialBuffer[0]=='y') {
    if(SerialBuffer[1]==' ') {
      //  This is a command to set the reverse flag
      if(SerialBuffer[2]=='1') {
        //  only do this if we are actually changing
        if(!DomeIsReversed) {
          DomeIsReversed=true;
          Dome.SaveConfig();
          accelStepper.setPinsInverted(false,false,false);
        }
      } else {
        if(DomeIsReversed) {
          DomeIsReversed=false;
          Dome.SaveConfig();
          accelStepper.setPinsInverted(true,false,false);
        }
      }
    }
    Computer.print("Y ");
    Computer.println(DomeIsReversed);
    
  }
  //  query/set for shutter hibernate timer
  if(SerialBuffer[0]=='r') {
    if(SerialBuffer[1]==' ') {
      unsigned long int newtimer;
      newtimer=atol(&SerialBuffer[1]);
      //Debug.println("Setting timer to ");
      //Debug.println(newtimer);
      Wireless.print("h ");
      Wireless.println(newtimer);
      //ShutterHibernateTimer=0;  //  trigger a query to see it stuck
    }
    Computer.print("R ");
    Computer.println(ShutterHibernateTimer);
  }
  /*  restart xbee wireless */
  if(SerialBuffer[0]=='w') {
    Computer.println("W");
    ConfigureWireless();
  }
  
  /* get firmware version */
  if(SerialBuffer[0]=='v') {
    Computer.print("VNexDome V ");
    Computer.print(VERSION_MAJOR);
    Computer.print(".");
    Computer.print(VERSION_MINOR);
    if(ShutterVersion[0] != 0) {
      Computer.print(" NexShutter V ");
      Computer.println(ShutterVersion);
    }
    Computer.println("");
  }

  SerialPointer=0;
  return;
}

void IncomingSerialChar(char a)
{

  if((a=='\n')||(a=='\r')) {
    //  dont process an empty line feed
    if(SerialPointer > 0) {
      ProcessSerialCommand();
    }
    memset(SerialBuffer,0,MY_SERIAL_BUFFER_SIZE);
    SerialPointer=0;
    return;
  }
  
  SerialBuffer[SerialPointer]=a;
  SerialPointer++;
  if(SerialPointer==MY_SERIAL_BUFFER_SIZE) {
    //  we are going to overflow the buffer
    //  erase it and start over
    memset(SerialBuffer,0,MY_SERIAL_BUFFER_SIZE);
    SerialPointer=0;
  } else {
    SerialBuffer[SerialPointer]=0;
  }
  return;
}

/*
 * Stuff we need for handling communication with the shutter
 * 
 */

void ConfigureWireless()
{
  //  do nothing for now
  //return;
  //Debug.println("Sending + to xbee");
  delay(1100);
  DoingWirelessConfig=true;
  WirelessConfigState=0;
  memset(WirelessBuffer,0,MY_SERIAL_BUFFER_SIZE);
  WirelessPointer=0;
  Wireless.print("+++");
}

void ProcessShutterData()
{
  //Debug.println(WirelessBuffer);
  if(WirelessBuffer[0]=='O') {
    if(WirelessBuffer[1]=='K') {
      //  The xbee unit is ready for configuration
      FoundXBee=true;
      if(DoingWirelessConfig) {
        switch(WirelessConfigState) {
          case 0:
            //  We could do the whole setup in just one command
            //  but it gives back the same number of OK responses
            //  So we have to parse out every OK anyways, may as well just
            //  do one setting per OK return
            //Wireless.println("ATID5555,CE1,PL0,SM4,SP5DC,ST100,CN");
            Wireless.println("ATID5555");
            //Debug.println("Setting wireless id");
            break;
         
          case 1:
            Wireless.println("ATCE1");
            //Debug.println("Setting cordinator");
            break;
            
         case 2:
            Wireless.println("ATPL0");
            //Debug.println("Setting Power");
            break;
            
          case 3:
            Wireless.println("ATSM4");
            //Wireless.println("ATSM0");
            //Debug.println("Setting sleep mode");
            break;
          
          case 4:
            //  set sleep period, this is the co-ordinator
            //  so it defines how long we will hold a message
            //  for the endpoint
            Wireless.println("ATSP5DC");
            //Debug.println("Setting sleep period");
            break;
            
          case 5:
            Wireless.println("ATST100");
            //Debug.println("Setting sleep wait time");
            break;
             
           case 6:
            Wireless.println("ATCN");
            //Debug.println("Exit command mode");
            break;
            
         default:
            //  Finished configuring, now see if the shutter is alive
            Wireless.println("s");
            //Debug.println("Wireless config finished");
            DoingWirelessConfig=false;
            break;
        }
        WirelessConfigState++;
      }
      //Debug.println("Clear wireless buffer");
      memset(WirelessBuffer,0,MY_SERIAL_BUFFER_SIZE);
      WirelessPointer=0;
      return;
    }
  }
  // update our timer for the keep alive routines
  if(!ShutterAlive) {
    if(WirelessBuffer[0]=='S') {
      //Debug.println(WirelessBuffer);
      //Debug.println("Shutter woke up");
      ShutterAlive=true;
    }
  }
  if(ShutterAlive) {
    if(WirelessBuffer[0]=='S') {
      long int newShutterState;
      LastShutterResponse=millis();
      LastShutterKeepAlive=millis();  //  mark this time for our keepalive routines as well
      //  the first character in the response is the shutter state
      //ShutterQueryTime=SHUTTER_SLEEP_WAIT;
      if(WirelessBuffer[2]=='0') ShutterQueryTime=SHUTTER_AWAKE_WAIT;
      else ShutterQueryTime=SHUTTER_SLEEP_WAIT;
      switch(WirelessBuffer[1]) {
        case 'C':
          newShutterState=SHUTTER_STATE_CLOSED;
          break;
        case 'O':
          newShutterState=SHUTTER_STATE_OPEN;
          break;
        case 'P':
          newShutterState=SHUTTER_STATE_OPENING;
          ShutterQueryTime=SHUTTER_AWAKE_WAIT;
          break;
        case 'D':
          newShutterState=SHUTTER_STATE_CLOSING;
          ShutterQueryTime=SHUTTER_AWAKE_WAIT;
          break;
        default:
          newShutterState=SHUTTER_STATE_UNKNOWN;
          break;
        //default:
        //  ShutterState=SHUTTER_STATE_NOT_CONNECTED;
        //  break;
      }
      if(newShutterState != ShutterState) {
        ShutterState=newShutterState;
        //Debug.print("Shutter State ");
        //Debug.println(ShutterState);
      }
      //  shutter has responded with a status
      //  lets ask it for a position
      if((ShutterState != SHUTTER_STATE_OPENING)&&(ShutterState != SHUTTER_STATE_CLOSING))Wireless.println("p");
    }
    if(WirelessBuffer[0]=='P') {
      ShutterPosition=atof(&WirelessBuffer[1]);
      //Debug.println("Shutter Position ");
      //Debug.println(q);
      //  We got a position
      //  lets ask for battery voltage
      Wireless.println("b");
    }
    if(WirelessBuffer[0]=='B') {
      char *ptr;
      ShutterBatteryVolts=atoi(&WirelessBuffer[1]);
      //  now find the low voltage cutoff if it exists
      ptr=&WirelessBuffer[2];
      while((ptr[0]!=0)&&(ptr[0]!=' ')&&(ptr[0]!=0x0a)&&(ptr[0]!=0x0d)) ptr++;
      if(ptr[0]==' ') {
        LowVoltCutoff=atoi(&ptr[0]);
      }
      if(ShutterHibernateTimer==0) {
        Wireless.println("h");
      }
      if(ShutterVersion[0]==0) {
        //  if we dont have a version for the shutter
        //  fetch it now
        //Debug.println("Fetch Shutter Version");
        Wireless.println("v");
      }
     }
    if(WirelessBuffer[0]=='V') {
      //Debug.print("Shutter Version ");
      //Debug.println(WirelessBuffer);
      memcpy(ShutterVersion,&WirelessBuffer[12],4);
      //Debug.println(ShutterVersion);
    }
    if(WirelessBuffer[0]=='H') {
      if(WirelessBuffer[1]==' ') {
        ShutterHibernateTimer=atol(&WirelessBuffer[1]);
      }
    }   
  }

  // clear the buffer now that it's processed
  memset(WirelessBuffer,0,MY_SERIAL_BUFFER_SIZE);
  WirelessPointer=0;
  
  return;
}

void IncomingWirelessChar(char a)
{
  //Debug.print(a);
  if((a=='\n')||(a=='\r')) {
    return ProcessShutterData();
  }
  //Debug.println((int)a);
  WirelessBuffer[WirelessPointer]=a;
  WirelessPointer++;
  if(WirelessPointer==MY_SERIAL_BUFFER_SIZE) {
    memset(WirelessBuffer,0,MY_SERIAL_BUFFER_SIZE);
    WirelessPointer=0;
  } else {
    WirelessBuffer[WirelessPointer]=0;
  }
  if(a < 0) {
    Computer.println("Wireless Clearing garbage");
    WirelessPointer=0;
    WirelessBuffer[0]=0;
  }
  //Debug.print(a);
  return;
}

unsigned long int LastBatteryCheck=0;

int CheckBattery()
{
  int volts;

  volts=analogRead(VPIN);
  //Debug.println(volts);
  volts=volts/2;
  volts=volts*3;
  //v=(float)volts/(float)100;
  BatteryVolts=volts;
  //Debug.println(volts);
  return 0;  
}

#ifdef ONCUE_OCS
void NexDomeLoop() {
#else
void loop() {
#endif
  int buttonstate;

  if(HomeSensor) {
    float h;
    //Dome.AtHome();
    //Debug.print("AtHome ");
    //Debug.println(Dome.GetHeading());

    //  we should re-sync on this sensor detect
    //  but just calling sync when it's in motion messes up all the
    //  state machines
    //Dome.Sync(Dome.HomeAzimuth);
    //  So instead, we save away how much our heading error is off by
    //  And apply the correction when we stop turning
      h=Dome.GetHeading();
      HeadingError=Dome.HomeAzimuth-h;
      //  if home position is within a degree of zero
      //  need to make an adjustment for rollover
      if(HeadingError < -359) HeadingError=HeadingError+360;
      Dome.AtHome();
     
    HomeSensor=false;
  }
  if(Dome.FindingHome) {
    if(SenseRising) {
      Dome.MoveTo(Dome.CurrentPosition()+5000);
    } else {
      Dome.MoveTo(Dome.CurrentPosition()-5000);
    }
  }
  //  Now things are all initialized, we just get down to the nitty gritty of running a dome
  //  lets check and see if any buttons are pushed
  buttonstate=CheckButtons();
  if(buttonstate != -1) {
    //  if we have had motion commanded over the serial port
    //  abort it and use the on board buttons as definitive
    if(buttonstate != 0) SerialTarget=false;
    switch(buttonstate) {
      case 1:
        Dome.MoveTo(Dome.CurrentPosition()+5000);
        break;
      case 2:
        Dome.MoveTo(Dome.CurrentPosition()-5000);
        break;
      default:
        //  Dont stop the dome if we have motion driven by incoming
        //  serial data stream
        if(!SerialTarget) Dome.Stop();
        break;
    }
  }

  //  is there any data from the host computer to process
  if(Computer.available()) {
    IncomingSerialChar(Computer.read());
  }

  //  now see if there is any data from the wireless to process
  if(Wireless.available()) {
    char a;
    a=Wireless.read();
    IncomingWirelessChar(a);
  }

  if(Dome.Active)
  {
    //Debug.println("r");
    Dome.Run();
  } else {
    //  When the dome is not moving
    if((HeadingError > 1.0) || (HeadingError < -1.0)) {
      float pos;

      //Debug.print("Heading correction ");
      //Debug.println(HeadingError);
      
      pos=Dome.GetHeading();
      pos=pos+HeadingError;
      Dome.Sync(pos);
      LastHeadingError=HeadingError;
      HeadingError=0;
    }
    //Debug.println("s");    
    //  we need to do a keep alive with the shutter
    //  And confirm it's still alive and running
    //  And we should check on battery voltage once in a while too

    unsigned long int now;
    now=millis();
    if ((long int)(now - LastShutterKeepAlive) > ShutterQueryTime) {
      CheckBattery();
      if(!FoundXBee) {
        ConfigureWireless();
      } else {
        //  send shutter a status query
        //Debug.println("Sending shutter status request");
        Wireless.println("s");
        //Wireless.println(millis());
        //  and get the battery voltage from the shutter
        //Wireless.println("b");
      }
      //Wireless.println(AliveCount++);
      LastShutterKeepAlive=now;
    }

    if(ShutterAlive) {
      //  Now lets see if it has timed out on responses, ie, gone away for some reason
      if ((long int)(now - LastShutterResponse) > 60000) {
        //  we have been 60 seconds without a response of any type
        //  we have to make sure this timing does not conflict with a keep-alive
        if ((long int)(now-LastShutterKeepAlive) < 5000) {
          //Debug.println("Waiting on wireless reset");
        } else {
          ShutterAlive=false;
          ShutterVersion[0]=0;
          ShutterState=SHUTTER_STATE_NOT_CONNECTED;
          Computer.println("shutter asleep");
          ConfigureWireless();
          LastShutterResponse=0;
        }
      }
    }
    
    // the serial target flag gets sent any time we get a motion command from the computer
    // and is the default at startup, so if we come up from a power outage safeties will kick in
    if(SerialTarget) {
      //  we are being driven by a computer
      unsigned long int now;
      now=millis();
      if((long int)(now - LastCommandTime) > 120000) {
        //  we have gone more than 2 minutes with nothing from the host computer
        //  if our shutter is not closed, and not closing, but we are talking to it
        //  close it
        if((ShutterState != SHUTTER_STATE_CLOSED)&&(ShutterState != SHUTTER_STATE_CLOSING)&&(ShutterState != SHUTTER_STATE_NOT_CONNECTED)) {
          //  we need to close the shutter
          //Debug.println("Drop dead close shutter");
          //  It doesn't hurt to print to the computer at this point
          //  because there are no programs processing anyways
          //  but dont send close commands endlessly either
          if(ShutterPosition < -500) {
            //  if we get here, we have been sending close commands to the shutter
            //  but it has not found the closed sensor
            //  We really shouldn't keep sending close commands at this point
            //  it'll just run stalled motors and run down the battery
            Computer.print("Shutter seems stalled at ");
            Computer.println(ShutterPosition);
            //  we cant do much at this point
            //  but we dont want to keep sending data to the host endlessly
            //  lets just reset the timeout state machine here
            ShutterState=SHUTTER_STATE_UNKNOWN;
            LastCommandTime=millis();
            //delay(1000);
            
          } else {
            //  It's time to
            Computer.println("  Close Shutter on host timeout");
            Wireless.println("c");
            //  shutter is almost certainly hibernated, so, put a delay between these
            //  it will eventually get the message
            delay(500);
          }
        }
      }
    }
  }
}

