#include "Arduino.h"
#include "types.h"
#include "Sensors.h"
#include "Algorithm.h"
#include "IMU.h"

void getEstimatedAttitude();


void computeIMU () {
  uint8_t axis;
  static int16_t gyroADCprevious[3] = {0,0,0};
  static int16_t gyroADCinter[3];

  uint16_t timeInterleave = 0;
  ACC_getADC();
  getEstimatedAttitude();
  Gyro_getADC();
  for (axis = 0; axis < 3; axis++)
    gyroADCinter[axis] =  imu.gyroADC[axis];
  timeInterleave=micros();
  annexCode();
  uint8_t t=0;
  while((int16_t)(micros()-timeInterleave)<650) t=1; //empirical, interleaving delay between 2 consecutive reads
  #ifdef LCD_TELEMETRY
    if (!t) annex650_overrun_count++;
  #endif
  Gyro_getADC();
  for (axis = 0; axis < 3; axis++) {
    gyroADCinter[axis] =  imu.gyroADC[axis]+gyroADCinter[axis];
    // empirical, we take a weighted value of the current and the previous values
    imu.gyroData[axis] = (gyroADCinter[axis]+gyroADCprevious[axis])/3;
    gyroADCprevious[axis] = gyroADCinter[axis]>>1;
    //if (!ACC) imu.accADC[axis]=0;
  }

}


#define ACC_LPF_FACTOR 4 // that means a LPF of 16

#define GYR_CMPF_FACTOR 10 //  that means a CMP_FACTOR of 1024 (2^10)

#define GYR_CMPFM_FACTOR 8 // that means a CMP_FACTOR of 256 (2^8)



typedef struct  {
  int32_t X,Y,Z;
} t_int32_t_vector_def;

typedef struct  {
  uint16_t XL; int16_t X;
  uint16_t YL; int16_t Y;
  uint16_t ZL; int16_t Z;
} t_int16_t_vector_def;

// note: we use implicit first 16 MSB bits 32 -> 16 cast. ie V32.X>>16 = V16.X
typedef union {
  int32_t A32[3];
  t_int32_t_vector_def V32;
  int16_t A16[6];
  t_int16_t_vector_def V16;
} t_int32_t_vector;

//return angle , unit: 1/10 degree
int16_t _atan2(int32_t y, int32_t x){
  float z = y;
  int16_t a;
  uint8_t c;
  c = abs(y) < abs(x);
  if ( c ) {z = z / x;} else {z = x / z;}
  a = 2046.43 * (z / (3.5714 +  z * z));
  if ( c ){
   if (x<0) {
     if (y<0) a -= 1800;
     else a += 1800;
   }
  } else {
    a = 900 - a;
    if (y<0) a -= 1800;
  }
  return a;
}

float InvSqrt (float x){ 
  union{  
    int32_t i;  
    float   f; 
  } conv; 
  conv.f = x; 
  conv.i = 0x5f1ffff9 - (conv.i >> 1); 
  return conv.f * (1.68191409f - 0.703952253f * x * conv.f * conv.f);
}

// signed16 * signed16
// 22 cycles
// http://mekonik.wordpress.com/2009/03/18/arduino-avr-gcc-multiplication/
#define MultiS16X16to32(longRes, intIn1, intIn2) \
asm volatile ( \
"clr r26 \n\t" \
"mul %A1, %A2 \n\t" \
"movw %A0, r0 \n\t" \
"muls %B1, %B2 \n\t" \
"movw %C0, r0 \n\t" \
"mulsu %B2, %A1 \n\t" \
"sbc %D0, r26 \n\t" \
"add %B0, r0 \n\t" \
"adc %C0, r1 \n\t" \
"adc %D0, r26 \n\t" \
"mulsu %B1, %A2 \n\t" \
"sbc %D0, r26 \n\t" \
"add %B0, r0 \n\t" \
"adc %C0, r1 \n\t" \
"adc %D0, r26 \n\t" \
"clr r1 \n\t" \
: \
"=&r" (longRes) \
: \
"a" (intIn1), \
"a" (intIn2) \
: \
"r26" \
)

int32_t  __attribute__ ((noinline)) mul(int16_t a, int16_t b) {
  int32_t r;
  MultiS16X16to32(r, a, b);
  //r = (int32_t)a*b; without asm requirement
  return r;
}

// Rotate Estimated vector(s) with small angle approximation, according to the gyro data
void rotateV32( t_int32_t_vector *v,int16_t* delta) {
  int16_t X = v->V16.X;
  int16_t Y = v->V16.Y;
  int16_t Z = v->V16.Z;

  v->V32.Z -=  mul(delta[ROLL]  ,  X)  + mul(delta[PITCH] , Y);
  v->V32.X +=  mul(delta[ROLL]  ,  Z)  - mul(delta[YAW]   , Y);
  v->V32.Y +=  mul(delta[PITCH] ,  Z)  + mul(delta[YAW]   , X);
}

static int16_t accZ=0;

void getEstimatedAttitude(){
  uint8_t axis;
  int32_t accMag = 0;
  float scale;
  int16_t deltaGyroAngle16[3];
  static t_int32_t_vector EstG = {0,0,(int32_t)ACC_1G<<16};
  #if MAG
    static t_int32_t_vector EstM;
  #else
    static t_int32_t_vector EstM = {0,(int32_t)1<<24,0};
  #endif
  static uint32_t LPFAcc[3];
  float invG; // 1/|G|
  static int16_t accZoffset = 0;
  int32_t accZ_tmp=0;
  static uint16_t previousT;
  uint16_t currentT = micros();

  // unit: radian per bit, scaled by 2^16 for further multiplication
  // with a delta time of 3000 us, and GYRO scale of most gyros, scale = a little bit less than 1
  scale = (currentT - previousT) * (GYRO_SCALE * 65536);
  previousT = currentT;

  // Initialization
  for (axis = 0; axis < 3; axis++) {
    // valid as long as LPF_FACTOR is less than 15
    imu.accSmooth[axis]  = LPFAcc[axis]>>ACC_LPF_FACTOR;
    LPFAcc[axis]      += imu.accADC[axis] - imu.accSmooth[axis];
    // used to calculate later the magnitude of acc vector
    accMag   += mul(imu.accSmooth[axis] , imu.accSmooth[axis]);
    // unit: radian scaled by 2^16
    // imu.gyroADC[axis] is 14 bit long, the scale factor ensure deltaGyroAngle16[axis] is still 14 bit long
    deltaGyroAngle16[axis] = imu.gyroADC[axis]  * scale;
  }

  // we rotate the intermediate 32 bit vector with the radian vector (deltaGyroAngle16), scaled by 2^16
  // however, only the first 16 MSB of the 32 bit vector is used to compute the result
  // it is ok to use this approximation as the 16 LSB are used only for the complementary filter part
  rotateV32(&EstG,deltaGyroAngle16);
  rotateV32(&EstM,deltaGyroAngle16);

  // Apply complimentary filter (Gyro drift correction)
  // If accel magnitude >1.15G or <0.85G and ACC vector outside of the limit range => we neutralize the effect of accelerometers in the angle estimation.
  // To do that, we just skip filter, as EstV already rotated by Gyro
  for (axis = 0; axis < 3; axis++) {
    if ( (int16_t)(0.85*ACC_1G*ACC_1G/256) < (int16_t)(accMag>>8) && (int16_t)(accMag>>8) < (int16_t)(1.15*ACC_1G*ACC_1G/256) )
      EstG.A32[axis] += (int32_t)(imu.accSmooth[axis] - EstG.A16[2*axis+1])<<(16-GYR_CMPF_FACTOR);
    accZ_tmp += mul(imu.accSmooth[axis] , EstG.A16[2*axis+1]);
    #if MAG
      EstM.A32[axis]  += (int32_t)(imu.magADC[axis] - EstM.A16[2*axis+1])<<(16-GYR_CMPFM_FACTOR);
    #endif
  }
  
  if (EstG.V16.Z > ACCZ_25deg)
    f.SMALL_ANGLES_25 = 1;
  else
    f.SMALL_ANGLES_25 = 0;

  // Attitude of the estimated vector
  int32_t sqGX_sqGZ = mul(EstG.V16.X,EstG.V16.X) + mul(EstG.V16.Z,EstG.V16.Z);
  invG = InvSqrt(sqGX_sqGZ + mul(EstG.V16.Y,EstG.V16.Y));
  att.angle[ROLL]  = _atan2(EstG.V16.X , EstG.V16.Z);
  att.angle[PITCH] = _atan2(EstG.V16.Y , InvSqrt(sqGX_sqGZ)*sqGX_sqGZ);

  //note on the second term: mathematically there is a risk of overflow (16*16*16=48 bits). assumed to be null with real values
  att.heading = _atan2(
    mul(EstM.V16.Z , EstG.V16.X) - mul(EstM.V16.X , EstG.V16.Z),
    (EstM.V16.Y * sqGX_sqGZ  - (mul(EstM.V16.X , EstG.V16.X) + mul(EstM.V16.Z , EstG.V16.Z)) * EstG.V16.Y)*invG );
 
  att.heading /= 10;



  // projection of ACC vector to global Z, with 1G subtructed
  // Math: accZ = A * G / |G| - 1G
  accZ = accZ_tmp *  invG;
  if (!f.ARMED) {
    accZoffset -= accZoffset>>3;
    accZoffset += accZ;
  }  
  accZ -= accZoffset>>3;
}

#define UPDATE_INTERVAL 25000    // 40hz update rate (20hz LPF on acc)
#define BARO_TAB_SIZE   21

#define ACC_Z_DEADBAND (ACC_1G>>5) // was 40 instead of 32 now

#define applyDeadband(value, deadband)  \
  if(abs(value) < deadband) {           \
    value = 0;                          \
  } else if(value > 0){                 \
    value -= deadband;                  \
  } else if(value < 0){                 \
    value += deadband;                  \
  }
