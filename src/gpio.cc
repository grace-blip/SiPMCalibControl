/**
 * @file gpio.cc
 * @author Yi-Mu Chen
 * @brief This is the handling of the GPIO interface of a raspberry PI that the
 * control program is suppose to run on.
 *
 * @class GPIO
 * @ingroup hardware
 * @brief Interface to the GPIO hardware interfaces.
 *
 * To reduce the number of external dependencies, we will be using the UNIX
 * `/sys` interface for direct access of the underlying system, which allows
 * for fast (microsecond level) timing precision, while still given decent
 * levels of human-readable interface abstraction. This will mean that the
 * additional system permission will need to be setup manually, rather than
 * through the packages. System permission for installation on calibration
 * hardware are kept in the `/external/rules` directory. See the install
 * instructions to learn more about how the permission need to be setup.
 *
 * Three sub-interfaces are implemented in this file:
 * - A direct GPIO interface for simple 1/0 outputs, such as for the trigger
 *   and sub-system switches.
 * - The PWM system for the systems voltage control.
 * - The I2C interface used to handle a 16bit ADC converter for DC readout.
 *   This is mainly used to monitor the system sensors like temperature and
 *   voltage drifts. To avoid overhead of acquiring ADC readout (which is one
 *   of the slowest interfaces of the control system), once the I2C interface
 *   is initialized, the readout is continuously flushed to a buffer in a
 *   separate thread, and retrieved whenever requested.
 *
 * The GPIO class will assume that all systems are present, if any one system
 * fails, then all interfaces should be shutdown. This is by design, as
 * adjusting GPIO/PWM settings on typical laptops and desktops is dangerous and
 * can potentially break the machine. The user will need to be very explicit
 * about hardware permission settings to be able to execute these routines.
 *
 * To ensure that the gantry control program is the only process on the system
 * that is using the control pins, all file descriptors will be locked on
 * opening. If anything fails to be locked uniquely to the control program
 * instance, then an exceptions is raised.

 *
 * Physical Pin locations:
 * - PWM Channel 0 is Physical PIN 12 (BWM pin 1/ALT5 mode in `gpio readall`
 * - PWM Channel 1 is Physical PIN 35 (BWM pin 24/ALT5 mode in `gpio readall`)
 *
 * List of references:
 * - General purpose input/ouput manipulation using `/sysfs`: [here][gpiosys]
 * - PWM manipulation (command-line piping): [here][pwmsys]
 * - I2C interface ofr ADS1115 ADC [here][adcsys]
 *
 * [adcsys]: http://www.bristolwatch.com/rpi/ads1115.html [gpiosys]:
 * https://www.ics.com/blog/gpio-programming-using-sysfs-interface [pwmsys]:
 * https://jumpnowtek.com/rpi/Using-the-Raspberry-Pi-Hardware-PWM-timers.html
 *
 */
#include "gpio.hpp"
#include "logger.hpp"

#include <cmath>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <deque>
#include <fmt/printf.h>
#include <queue>
#include <stdexcept>

static const std::string DeviceName = "GPIO";

/**
 * @brief Opening a file with a lock to ensure the program is the only process on
 * the system that is using the path.
 *
 * Mainly following the solution given [here][ref]. In the case that the file
 * descriptor cannot be opened or the lock instance cannot be generated, the
 * existing file descriptor will be closed and a exception will be raised. Notice
 * that the system lock will automatically be removed when the corresponding file
 * descriptor is closed.
 *
 * [ref]: https://stackoverflow.com/questions/1599459/optimal-lock-file-method
 */
static int
open_with_lock( const char* path, int mode )
{
  int fd = open( path, mode );
  if( fd == GPIO::OPEN_FAILED ){
    throw device_exception( DeviceName,
                            fmt::sprintf( "Failed to open path [%s]", path ) );
  }

  // The _lock will be non-zero if the processes cannot create the lock instance
  int lock = flock( fd, LOCK_EX | LOCK_NB );
  if( lock ){
    close( fd );
    fd = -1;
    throw device_exception( DeviceName,
                            fmt::sprintf( "Failed to lock path [%s]", path ));
  }
  return fd;
}


/********************************************************************************
 *
 * # THE GPIO CONTROL INTERFACE
 *
 *******************************************************************************/

/**
 * @brief Initialization a PIN for read or write.
 *
 * Notice that the pin index is not the physical pin index, but rather the BCM
 * pin index. Find out the correspondence using wiringPI's `gpio readall`
 * command. The return value will be the successfully opened file descriptor.
 */
int
GPIO::InitGPIOPin( const int pin, const unsigned direction )
{
  static constexpr unsigned buffer_length = 35;
  unsigned                  write_length;
  char                      buffer[buffer_length];
  int                       fd = open_with_lock( "/sys/class/gpio/export",
                                                 O_WRONLY );
  write_length = snprintf( buffer, buffer_length, "%u", pin );
  write( fd, buffer, write_length );
  close( fd );

  // Small pause for system settings to settle
  std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

  // Getting the direction path
  std::string path = fmt::sprintf( "/sys/class/gpio/gpio%d/direction", pin );

  // Waiting for the /sysfs to generated the corresponding file
  while( access( path.c_str(), F_OK ) == -1 ){
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  }
  fd =
    ( direction == READ ) ? open_with_lock( path.c_str(),
                                            O_WRONLY ) : open_with_lock(
      path.c_str(),
      O_WRONLY );
  int status = write( fd,
                      direction == READ ? "in" : "out",
                      direction == READ ? 2    : 3 );
  if( status == IO_FAILED ){
    throw device_exception( DeviceName,
                            fmt::sprintf(
                              "Failed to set gpio [%d] direction!",
                              pin ) );
  }
  close( fd );

  // Opening GPIO PIN
  path = fmt::sprintf( "/sys/class/gpio/gpio%d/value", pin );
  fd   =
    ( direction == READ ) ? open_with_lock( path.c_str(),
                                            O_RDONLY ) : open_with_lock(
      path.c_str(),
      O_WRONLY );
  return fd;
}


/**
 * @brief Reading from a GPIO initialized file descriptor
 *
 * The value would be either 1 or 0 (integer) depending of if the pin detects
 * high or low voltage values.
 */
int
GPIO::GPIORead( const int fd )
{
  char value_str[3];
  if( read( fd, value_str, 3 ) == IO_FAILED ){
    throw device_exception( DeviceName, "Failed to read gpio value!" );
  }
  return atoi( value_str );
}


/**
 * @brief Writing to a GPIO initialized file descriptor
 *
 * Writing either 1 or 0 (string) to the designated file descriptor. If
 * anything goes wrong raise an excpetion.
 */
void
GPIO::GPIOWrite( const int fd, const unsigned val )
{
  if( write( fd, LOW == val ? "0" : "1", 1 ) == IO_FAILED ){
    throw device_exception( DeviceName, "Failed to write gpio value!" );
  }
}


/**
 * @brief Closing the GPIO file descriptor.
 *
 * This is important for restarting the program, otherwise the GPIO sysfs will
 * be occupied by the non-existant closed program.
 */
void
GPIO::CloseGPIO( const int pin )
{
  static constexpr unsigned buffer_length = 3;
  unsigned                  write_length;
  char                      buffer[buffer_length];
  int                       fd = open_with_lock( "/sys/class/gpio/unexport",
                                                 O_WRONLY );
  write_length = snprintf( buffer, buffer_length, "%d", pin );
  write( fd, buffer, write_length );
  close( fd );
}


/**
 * @brief Generating N pulses with some time in between pulses.
 *
 * All pulses will have a high-time of 1 microsecond, and a w microsecond of
 * down time. The fastest pulse rate is about 100 microseconds.
 */
void
GPIO::Pulse( const unsigned n, const unsigned wait ) const
{
  if( gpio_trigger == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            "GPIO for trigger pin is not initialized" );
  }
  for( unsigned i = 0; i < n; ++i ){
    GPIOWrite( gpio_trigger, HI );
    std::this_thread::sleep_for( std::chrono::microseconds( 1 ) );
    GPIOWrite( gpio_trigger, LOW );
    std::this_thread::sleep_for( std::chrono::microseconds( wait ) );
  }
}


/**
 * @{
 * @brief Simple function for turning on and off the light pin.
 */
void
GPIO::LightsOn() const
{
  if( gpio_light == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            "GPIO for light pin is not initialized" );
  }
  GPIOWrite( gpio_light, HI );
}


void
GPIO::LightsOff() const
{
  if( gpio_light == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            "GPIO for light pin is not initialized" );
  }
  GPIOWrite( gpio_light, LOW );
}


/** @} */


/**
 * @{
 * @brief Simple function for turning on and off the spare pin
 */
void
GPIO::SpareOn() const
{
  if( gpio_spare == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            "GPIO for spare pin is not initialized" );
  }
  GPIOWrite( gpio_spare, HI );
}


void
GPIO::SpareOff() const
{
  if( gpio_spare == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            "GPIO for spare pin is not initialized" );
  }
  GPIOWrite( gpio_spare, LOW );
}


/** @} */


/********************************************************************************
 *
 * PWM SYSFS INTERFACE
 *
 *******************************************************************************/

/**
 * @brief Opening the file descriptors for PWM manipulation
 *
 * As the pwmchip is at a fixed location, no input is needed for
 * initialization. Some additional concerns:
 * - As the PWM requires some kernel time to become available. A very small
 *   loop will be used to open the variable file descriptors require to start
 *   the process.
 * - We will always attempt to lock the file descriptors to ensure that the
 *   program is the only processes using the specified physical pins. Failure
 *   to lock the file descriptors will raise an error.
 */
void
GPIO::InitPWM()
{
  // Flaaing the pwm_enable as open failed.
  pwm_enable[0] = OPEN_FAILED;
  pwm_enable[1] = OPEN_FAILED;
  int fd = open_with_lock( "/sys/class/pwm/pwmchip0/export", O_WRONLY );
  write( fd, "0", 1 );
  write( fd, "1", 1 );// Single write to enable interface
  close( fd );

  // Waiting for the sysfs to generated the corresponding file
  while( access( "/sys/class/pwm/pwmchip0/pwm0/enable", F_OK ) == OPEN_FAILED ){
    printinfo( DeviceName, "Waiting for /sys/class/pwm/pwmchip0/pwm0/enable" );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  }
  while( access( "/sys/class/pwm/pwmchip0/pwm1/enable", F_OK ) == OPEN_FAILED ){
    printinfo( DeviceName, "Waiting for /sys/class/pwm/pwmchip0/pwm1/enable" );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  }

  do{
    pwm_enable[0] = open( "/sys/class/pwm/pwmchip0/pwm0/enable", O_WRONLY );
    pwm_duty[0]   = open( "/sys/class/pwm/pwmchip0/pwm0/duty_cycle", O_WRONLY );
    pwm_period[0] = open( "/sys/class/pwm/pwmchip0/pwm0/period", O_WRONLY );
    pwm_enable[1] = open( "/sys/class/pwm/pwmchip0/pwm1/enable", O_WRONLY );
    pwm_duty[1]   = open( "/sys/class/pwm/pwmchip0/pwm1/duty_cycle", O_WRONLY );
    pwm_period[1] = open( "/sys/class/pwm/pwmchip0/pwm1/period", O_WRONLY );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  } while( pwm_enable[0] == UNOPENED || pwm_enable[0] == OPEN_FAILED );

  // Attempting to lock everything
  for( int fd :
       {pwm_enable[0], pwm_duty[0], pwm_period[0], pwm_enable[1], pwm_duty[1],
        pwm_period[1]} ){
    int lock = flock( fd, LOCK_EX | LOCK_NB );
    if( lock ){
      close( pwm_enable[0] );
      close( pwm_duty[0] );
      close( pwm_period[0] );
      close( pwm_enable[1] );
      close( pwm_duty[1] );
      close( pwm_period[1] );
      pwm_enable[0] = pwm_duty[0] = pwm_period[0] = pwm_enable[1] =
        pwm_duty[1] = pwm_period[1] = UNOPENED;
      throw device_exception( DeviceName, "Failed to lock PWM files" );
    }
  }
}


/**
 * @brief Closing the PWM interface. Important for restarts.
 */
void
GPIO::ClosePWM()
{
  if( pwm_enable[0] != UNOPENED ){
    for( unsigned channel = 0; channel <= 1; channel++ ){
      write( pwm_enable[channel], "0", 1 );
      close( pwm_enable[channel] );
      close( pwm_duty[channel] );
      close( pwm_period[channel] );
    }
    int fd = open( "/sys/class/pwm/pwmchip0/unexport", O_WRONLY );
    if( fd == OPEN_FAILED ){
      throw device_exception( DeviceName,
                              "Failed to open /sys/class/pwm/pwmchip0/unexport" );
    }
    write( fd, "0", 1 );
    write( fd, "1", 1 );
  }
}


/**
 * @brief Setting the PWM channel to a specific duty cycle and operation
 * frequency.
 *
 * Here we limit the frequency to 10kHz, as has been found that the PWM chip on
 * the raspberry pi runs into instability past this frequency, though the timing
 * can potentially be set down to the nanosecond. The duty cycle is then
 * converted to the corresponding time frame.
 *
 * One small note, as the ADC readout system is also used to monitor the PWM
 * voltage, in the case the PWM system is not available (for example for local
 * testing), the ADC readout array is filled with the estimated value (5000mV x
 * dutycycle) so that the dummy test can still see the duty cycle command being
 * invoked.
 */
void
GPIO::SetPWM( const unsigned c, const double dc, const double f )
{
  // Limiting range
  const float    frequency  = std::min( 1e5, f );
  const float    duty_cycle = std::min( 1.0, std::max( 0.0, dc ) );
  const unsigned channel    = std::min( unsigned(1), c );

  // Time is in units of nano seconds
  const unsigned period = 1e9 / frequency;
  const unsigned duty   = period * duty_cycle;
  char           duty_str[10];
  char           period_str[10];
  unsigned       duty_len   = sprintf( duty_str,    "%u", duty );
  unsigned       period_len = sprintf( period_str,  "%u", period );
  if( pwm_enable[channel] == OPEN_FAILED ){
    throw device_exception( DeviceName,
                            fmt::sprintf(
                              "Failed to open /sys/class/pwm/pwmchip%u settings",
                              c ) );
  } else if( pwm_enable[channel] == UNOPENED ){
    if( channel == 0 ){
      i2c_flush_array[2] = duty_cycle * 5000.0;
    } else if( channel == 1 ){
      i2c_flush_array[3] = duty_cycle * 5000.0;
    }
  } else {
    write( pwm_enable[channel], "0",        1          );
    write( pwm_period[channel], period_str, period_len );
    write( pwm_duty[channel],   duty_str,   duty_len   );
    write( pwm_enable[channel], "1",        1          );
  }

  // Storing the PWM value for external reference.
  pwm_duty_value[channel] = duty_cycle;
}


/**
 * @brief Reading out the duty cycle for a given channel.
 */
float
GPIO::GetPWM( const unsigned c )
{
  const unsigned channel = std::min( unsigned(1), c );
  return pwm_duty_value[channel];
}


/********************************************************************************
 *
 * I2C INTERFACE FOR THE ADS1115 ADC DC READOUT SYSTEM
 *
 *******************************************************************************/

/**
 * @{
 * @brief Static variables for setting to the ADC configurations.
 */
const uint8_t GPIO::ADS_RANGE_6V   = 0x0;
const uint8_t GPIO::ADS_RANGE_4V   = 0x1;
const uint8_t GPIO::ADS_RANGE_2V   = 0x2;
const uint8_t GPIO::ADS_RANGE_1V   = 0x3;
const uint8_t GPIO::ADS_RANGE_p5V  = 0x4;
const uint8_t GPIO::ADS_RANGE_p25V = 0x5;

const uint8_t GPIO::ADS_RATE_8SPS   = 0x0;
const uint8_t GPIO::ADS_RATE_16SPS  = 0x1;
const uint8_t GPIO::ADS_RATE_32SPS  = 0x2;
const uint8_t GPIO::ADS_RATE_64SPS  = 0x3;
const uint8_t GPIO::ADS_RATE_128SPS = 0x4;
const uint8_t GPIO::ADS_RATE_250SPS = 0x5;
const uint8_t GPIO::ADS_RATE_475SPS = 0x6;
const uint8_t GPIO::ADS_RATE_860SPS = 0x7;

/** @} */

/**
 * @brief Opening the I2C device as a slave and returning the corresponding file
 * descriptor.
 */
int
GPIO::InitI2C()
{
  int fd = open_with_lock( "/dev/i2c-1", O_RDWR );

  // connect to ADS1115 as i2c slave
  if( ioctl( fd, I2C_SLAVE, 0x48 ) == IO_FAILED ){
    throw device_exception( DeviceName,
                            fmt::sprintf(
                              "Error: Couldn't find i2c device on address [%d]!",
                              0x48 ));
  }
  return fd;
}


/**
 * @{
 * @brief Modifying the ADC settings via the configuration constant variables.
 */
void
GPIO::SetADCRange( const int range )
{
  if( adc_range  != range ){
    adc_range = range;
    PushADCSetting();
  }
}


void
GPIO::SetADCRate( const int rate )
{
  if( rate != adc_rate ){
    adc_rate = rate;
    PushADCSetting();
  }
}


/**
 * @details This is the routine that actually writes the configuration settings
 * to the I2D device. Notice we will always be using the continuous readout
 * operation mode.
 */
void
GPIO::PushADCSetting()
{
  const uint8_t channel         = ( adc_channel & 0x3 ) | ( 0x1 << 2 );
  const uint8_t range           = ( adc_range & 0x7 );
  const uint8_t rate            = ( adc_rate  & 0x7 );
  uint8_t       write_buffer[3] = {
    1,// First register bit is always 1,
    // Configuration byte 1
    // Always  | MUX bits     | PGA bits    | MODE (always continuous)
    // 1       | x    x    x  | x   x   x   | 0
    ( 0x1 << 7 | channel << 4 |  range << 1 | 0x0 ),

    // Configuration byte 0
    // DR bits |  COM BITS (Leave as default)
    // x x x   | 0 0  0 1 1
    ( rate << 5  | 0b00011 )};
  uint8_t       read_buffer[2] = {0};

  // Write and wait for OK signal.
  if( write( gpio_adc, write_buffer, 3 ) != 3 ){
    throw device_exception( DeviceName, "Error writing setting to i2C device" );
  }
  std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

  // Resetting to read mode
  read_buffer[0] = 0;
  if( write( gpio_adc, read_buffer, 1 ) != 1 ){
    throw device_exception( DeviceName,
                            "Error setting to i2C device to read mode" );
  }
}


/** @} */

/**
 * @brief Reading out the I2C interface at the current channel as a 16bit
 * number.
 *
 * Conversion is handled by the flushing loop.
 */
int16_t
GPIO::ADCReadRaw()
{
  uint8_t read_buffer[2] = {0};
  int16_t ans;
  read( gpio_adc, read_buffer, 2 );
  ans = read_buffer[0] << 8 | read_buffer[1];
  return ans;
}


/**
 * @brief The main loop for flushing the readout results into the buffer.
 *
 * Notice that the i2C readout will always be a single channel so the loop is
 * responsible for iterating the readout channel. The loop is continuously run
 * until the i2d_flush is set to false (when exiting the program or
 * re-initializing the i2c interface.)
 */
void
GPIO::FlushLoop( std::atomic<bool>& i2d_flush )
{
  while( i2c_flush == true ){
    if( gpio_adc >= NORMAL_PTR ){
      for( unsigned channel = 0; channel < 4; ++channel ){
        adc_channel = channel;
        try {// This is incase the GPIO interface is open but not addressable
          PushADCSetting();
          const int16_t adc   = ADCReadRaw();
          const uint8_t range = adc_range & 0x7;
          const float   conv  = range ==
                                ADS_RANGE_6V  ? 6144.0 / 32678.0 : range ==
                                ADS_RANGE_4V  ? 4096.0 / 32678.0 : range ==
                                ADS_RANGE_2V  ? 2048.0 / 32678.0 : range ==
                                ADS_RANGE_1V  ? 1024.0 / 32678.0 : range ==
                                ADS_RANGE_p5V ? 512.0 / 32678.0  : 256.0
                                / 32678.0;
          i2c_flush_array[channel] = adc * conv;
          std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        } catch( std::exception& e ){
          i2c_flush_array[0] = i2c_flush_array[0];
          i2c_flush_array[1] = i2c_flush_array[1];
          i2c_flush_array[2] = i2c_flush_array[2];
          i2c_flush_array[3] = i2c_flush_array[3];
        }
      }
    } else {
      i2c_flush_array[0] = i2c_flush_array[0];
      i2c_flush_array[1] = i2c_flush_array[1];
      i2c_flush_array[2] = i2c_flush_array[2];
      i2c_flush_array[3] = i2c_flush_array[3];
    }
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
  }
}


/**
 * @brief Starting the thread for running the i2c flushing loop.
 */
void
GPIO::InitI2CFlush()
{
  i2c_flush        = true;
  i2c_flush_thread = std::thread( [this]{
    this->FlushLoop( std::ref( i2c_flush ) );
  } );
}


/**
 * @brief Stopping the i2c flushing loop.
 */
void
GPIO::CloseI2CFlush()
{
  if( i2c_flush == true ){
    i2c_flush = false;
    i2c_flush_thread.join();
  }
}


/**
 * @brief High level function for reading the latest i2c readout voltage of a
 * given channel in mV.
 */
float
GPIO::ReadADC( const unsigned channel ) const
{
  return i2c_flush_array[channel];
}


/**
 * @brief Reference voltage (in mV) for the voltage readout conversion.
 */
void
GPIO::SetReferenceVoltage( const unsigned channel, const float val )
{
  reference_voltage[channel] = val;
}


/**
 * @brief Interpreting the voltage readout of the specified channel as the
 * temperature readout from a NTC thermistor.
 *
 * This function assumes that the ADC is reading the voltage between a 10K
 * B-3500 thermistor and a 10K resistor in series, with the thermistor being
 * grounded. We assume that the 700K input impedance of the ADC is negligible.
 * There reference voltage needs to be measured independently for an accurate
 * readout.
 *
 * The conversion is performed using the Steinhart-Hart equation:
 * - 1/T = 1/T0 + 1/B * ln(R/R0) The return will a temperature in units of C
 */
float
GPIO::ReadNTCTemp( const unsigned channel ) const
{
  // Standard values for NTC resistors used in circuit;
  static const float T_0 = 25+273.15;
  static const float R_0 = 10000;
  static const float B   = 3500;

  // Standard operation values for biasing circuit
  static const float R_ref = 10000;

  // Dynamic convertion
  const float V_total = reference_voltage[channel];
  const float v       = ReadADC( channel );
  const float R       = R_ref * v / ( V_total-v );
  return ( T_0 * B ) / ( B+T_0 * std::log( R / R_0 ) )-273.15;
}


/**
 * @brief Interpreting the voltage readout of the specified channel as the
 * temperature readout from a RTD platinum resistance thermometer.
 *
 * This function assumes that the ADC is reading the voltage between a 10K
 * platinum RTD and a 10K resistor in series, with the RTD being grounded. We
 * assume that the 700K input impedance of the ADC is negligible. There
 * reference voltage needs to be measured independently for an accurate
 * readout.
 *
 * The conversion is performed using the Linearity equation:
 * - R = R_0 (1 + a (T - T0)) The return will a temperature in units of C
 */
float
GPIO::ReadRTDTemp( const unsigned channel ) const
{
  // Typical value of RTDs in circuit
  static const float R_0 = 10000;
  static const float T_0 = 273.15;
  static const float a   = 0.003916;

  // standard operation values for biasing circuit
  static const float R_ref = 10000;

  // Dynamic conversion
  const float V_total = reference_voltage[channel];
  const float v       = ReadADC( channel );
  const float R       = R_ref * v / ( V_total-v );

  // Temperature conversion is simply
  // R = R_0 (1 + a (T - T0))
  return T_0+( R-R_0 ) / ( R_0 * a )-273.15;
}


/********************************************************************************
 *
 * ADDITIONAL CLASS HANDLERS
 *
 *******************************************************************************/


/**
 * @brief Construct a new GPIO::GPIO handler
 *
 * Here this is simply setting everything to a default values, and flagging all
 * used file descriptors as unopen. During construction time. No interfaces
 * will be activated.
 *
 */
GPIO::GPIO() : gpio_trigger( UNOPENED ),
  gpio_light               ( UNOPENED ),
  gpio_spare               ( UNOPENED ),
  gpio_adc                 ( UNOPENED ),
  adc_range                ( ADS_RANGE_4V ),
  adc_rate                 ( ADS_RATE_250SPS ),
  adc_channel              ( 0 ),
  i2c_flush                ( false )
{
  pwm_enable[0] = UNOPENED;
  pwm_duty[0]   = UNOPENED;
  pwm_period[0] = UNOPENED;
  pwm_enable[1] = UNOPENED;
  pwm_duty[1]   = UNOPENED;
  pwm_period[1] = UNOPENED;

  i2c_flush_array[0] = 2500.0;
  i2c_flush_array[1] = 2500.0;
  i2c_flush_array[2] = 2500.0;
  i2c_flush_array[3] = 2500.0;

  reference_voltage[0] = 5000.0;
  reference_voltage[1] = 5000.0;
  reference_voltage[2] = 5000.0;
  reference_voltage[3] = 5000.0;

  pwm_duty_value[0] = 0.5;
  pwm_duty_value[1] = 0.5;
}


/**
 * @brief Interface initialization
 *
 * The continuous readout system of the ADC interface will need to exists
 * regardless of whether a real i2C interface is present, so additional
 * exception handling needs to be done on C++ level before rethrowing the
 * exception (In the case that the i2c interface is not "real", the continuous
 * readout is simply the a continuous steam of what ever the current set value
 * for the PWM duty cycle is)
 */
void
GPIO::Init()
{
  try {
    gpio_light   = InitGPIOPin(   light_pin, WRITE );
    gpio_trigger = InitGPIOPin( trigger_pin, WRITE );
    gpio_spare   = InitGPIOPin(   spare_pin, WRITE );
    InitPWM();

    if( gpio_adc != UNOPENED ){
      CloseI2CFlush();
    }
    gpio_adc = InitI2C();
    if( gpio_adc != OPEN_FAILED && gpio_adc != UNOPENED ){
      PushADCSetting();
      InitI2CFlush();
    }
  } catch( std::runtime_error& e ){
    // For local testing, this start the I2C monitoring flush even if
    // Something failed, (The ADC readout will just be a random stream)
    InitI2CFlush();

    // Passing error message onto higher functions
    throw e;
  }
}


/**
 * @brief Verbose closing interface to make sure all interfaces has closed
 * properly.
 */
GPIO::~GPIO()
{
  // Turning off LED light when the process has ended.
  printdebug( DeviceName, "Closing GPIO pins for the light" );
  if( gpio_light >= NORMAL_PTR ){
    LightsOff();
    close( gpio_light );
    CloseGPIO( light_pin );
  }

  printdebug( DeviceName, "Closing GPIO pins for the trigger" );
  if( gpio_trigger >= NORMAL_PTR ){
    close( gpio_trigger );
    CloseGPIO( trigger_pin );
  }

  printdebug( DeviceName, "Closing GPIo pins for the PWM" );
  ClosePWM();

  printdebug( DeviceName, "Closing the I2C interface\n" );
  CloseI2CFlush();// Closing the flush interface regardless
  if( gpio_adc >= NORMAL_PTR ){
    close( gpio_adc );
  }
  printdebug( DeviceName, "All GPIO successfully shutdown\n" );
}


/**
 * @brief Checking that the 3 specified GPIO PINs are available.
 * @return false
 */
bool
GPIO::StatusGPIO() const
{
  return gpio_trigger >= NORMAL_PTR && gpio_light >= NORMAL_PTR &&
         gpio_spare   >= NORMAL_PTR;
}


/**
 * @brief Checking that the ADC/i2C interface is available.
 */
bool
GPIO::StatusADC() const
{
  return gpio_adc >= NORMAL_PTR;
}


/**
 * @brief Checking that the PWM interface is available.
 */
bool
GPIO::StatusPWM() const
{
  return pwm_enable[0] >= NORMAL_PTR && pwm_duty[0]   >= NORMAL_PTR &&
         pwm_period[0] >= NORMAL_PTR && pwm_enable[1] >= NORMAL_PTR &&
         pwm_duty[1]   >= NORMAL_PTR && pwm_period[1] >= NORMAL_PTR;
}


IMPLEMENT_SINGLETON( GPIO );
