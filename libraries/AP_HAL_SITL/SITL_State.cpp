/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <AP_HAL/AP_HAL.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL

#include "AP_HAL_SITL.h"
#include "AP_HAL_SITL_Namespace.h"
#include "HAL_SITL_Class.h"
#include "UARTDriver.h"
#include "Scheduler.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/select.h>

#include <AP_Param/AP_Param.h>

extern const AP_HAL::HAL& hal;

using namespace HALSITL;
using namespace SITL;

void SITL_State::_set_param_default(const char *parm)
{
    char *pdup = strdup(parm);
    char *p = strchr(pdup, '=');
    if (p == NULL) {
        printf("Please specify parameter as NAME=VALUE");
        exit(1);
    }
    float value = strtof(p+1, NULL);
    *p = 0;
    enum ap_var_type var_type;
    AP_Param *vp = AP_Param::find(pdup, &var_type);
    if (vp == NULL) {
        printf("Unknown parameter %s\n", pdup);
        exit(1);
    }
    if (var_type == AP_PARAM_FLOAT) {
        ((AP_Float *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT32) {
        ((AP_Int32 *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT16) {
        ((AP_Int16 *)vp)->set_and_save(value);
    } else if (var_type == AP_PARAM_INT8) {
        ((AP_Int8 *)vp)->set_and_save(value);
    } else {
        printf("Unable to set parameter %s\n", pdup);
        exit(1);
    }
    printf("Set parameter %s to %f\n", pdup, value);
    free(pdup);
}


/*
  setup for SITL handling
 */
void SITL_State::_sitl_setup(const char *home_str)
{
#ifndef __CYGWIN__
    _parent_pid = getppid();
#endif
    _rcout_addr.sin_family = AF_INET;
    _rcout_addr.sin_port = htons(_rcout_port);
    inet_pton(AF_INET, _fdm_address, &_rcout_addr.sin_addr);

#ifndef HIL_MODE
    _setup_fdm();
#endif
    fprintf(stdout, "Starting SITL input\n");

    // find the barometer object if it exists
    _sitl = (SITL::SITL *)AP_Param::find_object("SIM_");
    _barometer = (AP_Baro *)AP_Param::find_object("GND_");
    _ins = (AP_InertialSensor *)AP_Param::find_object("INS_");
    _compass = (Compass *)AP_Param::find_object("COMPASS_");
    _terrain = (AP_Terrain *)AP_Param::find_object("TERRAIN_");
    _optical_flow = (OpticalFlow *)AP_Param::find_object("FLOW");

    if (_sitl != NULL) {
        // setup some initial values
#ifndef HIL_MODE
        _update_barometer(100);
        _update_ins(0, 0, 0, 0, 0, 0, 0, 0, -9.8, 0, 100);
        _update_compass(0, 0, 0);
        _update_gps(0, 0, 0, 0, 0, 0, false);
#endif
        if (enable_gimbal) {
            gimbal = new Gimbal(_sitl->state);
        }
        if (enable_ADSB) {
            adsb = new ADSB(_sitl->state, home_str);
        }
    }

    if (_synthetic_clock_mode) {
        // start with non-zero clock
        hal.scheduler->stop_clock(1);
    }
}


#ifndef HIL_MODE
/*
  setup a SITL FDM listening UDP port
 */
void SITL_State::_setup_fdm(void)
{
    int one=1, ret;
    struct sockaddr_in sockaddr;

    memset(&sockaddr,0,sizeof(sockaddr));

#ifdef HAVE_SOCK_SIN_LEN
    sockaddr.sin_len = sizeof(sockaddr);
#endif
    sockaddr.sin_port = htons(_simin_port);
    sockaddr.sin_family = AF_INET;

    _sitl_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_sitl_fd == -1) {
        fprintf(stderr, "SITL: socket failed - %s\n", strerror(errno));
        exit(1);
    }

    /* we want to be able to re-use ports quickly */
    setsockopt(_sitl_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    ret = bind(_sitl_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (ret == -1) {
        fprintf(stderr, "SITL: bind failed on port %u - %s\n",
                (unsigned)ntohs(sockaddr.sin_port), strerror(errno));
        exit(1);
    }

    HALSITL::SITLUARTDriver::_set_nonblocking(_sitl_fd);
}
#endif


/*
  step the FDM by one time step
 */
void SITL_State::_fdm_input_step(void)
{
    static uint32_t last_pwm_input = 0;

    _fdm_input_local();

    /* make sure we die if our parent dies */
    if (kill(_parent_pid, 0) != 0) {
        exit(1);
    }

    if (_scheduler->interrupts_are_blocked() || _sitl == NULL) {
        return;
    }

    // simulate RC input at 50Hz
    if (AP_HAL::millis() - last_pwm_input >= 20 && _sitl->rc_fail == 0) {
        last_pwm_input = AP_HAL::millis();
        new_rc_input = true;
    }

    _scheduler->sitl_begin_atomic();

    if (_update_count == 0 && _sitl != NULL) {
        _update_gps(0, 0, 0, 0, 0, 0, false);
        _update_barometer(0);
        _scheduler->timer_event();
        _scheduler->sitl_end_atomic();
        return;
    }

    if (_sitl != NULL) {
        _update_gps(_sitl->state.latitude, _sitl->state.longitude,
                    _sitl->state.altitude,
                    _sitl->state.speedN, _sitl->state.speedE, _sitl->state.speedD,
                    !_sitl->gps_disable);
        _update_ins(_sitl->state.rollDeg, _sitl->state.pitchDeg, _sitl->state.yawDeg,
                    _sitl->state.rollRate, _sitl->state.pitchRate, _sitl->state.yawRate,
                    _sitl->state.xAccel, _sitl->state.yAccel, _sitl->state.zAccel,
                    _sitl->state.airspeed, _sitl->state.altitude);
        _update_barometer(_sitl->state.altitude);
        _update_compass(_sitl->state.rollDeg, _sitl->state.pitchDeg, _sitl->state.yawDeg);
        _update_flow();
    }

    // trigger all APM timers.
    _scheduler->timer_event();
    _scheduler->sitl_end_atomic();
}


void SITL_State::wait_clock(uint64_t wait_time_usec)
{
    while (AP_HAL::micros64() < wait_time_usec) {
        _fdm_input_step();
    }
}

#ifndef HIL_MODE
/*
  check for a SITL FDM packet
 */
void SITL_State::_fdm_input(void)
{
    ssize_t size;
    struct pwm_packet {
        uint16_t pwm[8];
    } pwm_pkt;

    size = recv(_sitl_fd, &pwm_pkt, sizeof(pwm_pkt), MSG_DONTWAIT);
    switch (size) {
    case sizeof(pwm_pkt): {
        // a packet giving the receiver PWM inputs
        uint8_t i;
        for (i=0; i<8; i++) {
            // setup the pwn input for the RC channel inputs
            if (pwm_pkt.pwm[i] != 0) {
                pwm_input[i] = pwm_pkt.pwm[i];
            }
        }
        break;
    }
    }
}


/*
  get FDM input from a local model
 */
void SITL_State::_fdm_input_local(void)
{
    Aircraft::sitl_input input;

    // check for direct RC input
    _fdm_input();

    // construct servos structure for FDM
    _simulator_servos(input);

    // update the model
    sitl_model->update(input);

    // get FDM output from the model
    sitl_model->fill_fdm(_sitl->state);
    _sitl->update_rate_hz = sitl_model->get_rate_hz();

    if (gimbal != NULL) {
        gimbal->update();
    }
    if (adsb != NULL) {
        adsb->update();
    }

    // update simulation time
    hal.scheduler->stop_clock(_sitl->state.timestamp_us);

    _synthetic_clock_mode = true;
    _update_count++;
}
#endif

/*
  apply servo rate filtering
  This allows simulation of servo lag
 */
void SITL_State::_apply_servo_filter(float deltat)
{
    if (_sitl->servo_rate < 1.0f) {
        // no limit
        return;
    }
    // 1000 usec == 90 degrees
    uint16_t max_change = deltat * _sitl->servo_rate * 1000 / 90;
    if (max_change == 0) {
        max_change = 1;
    }
    for (uint8_t i=0; i<SITL_NUM_CHANNELS; i++) {
        int16_t change = (int16_t)pwm_output[i] - (int16_t)last_pwm_output[i];
        if (change > max_change) {
            pwm_output[i] = last_pwm_output[i] + max_change;
        } else if (change < -max_change) {
            pwm_output[i] = last_pwm_output[i] - max_change;
        }
    }
}


/*
  create sitl_input structure for sending to FDM
 */
void SITL_State::_simulator_servos(Aircraft::sitl_input &input)
{
    static uint32_t last_update_usec;

    /* this maps the registers used for PWM outputs. The RC
     * driver updates these whenever it wants the channel output
     * to change */
    uint8_t i;

    if (last_update_usec == 0) {
        for (i=0; i<SITL_NUM_CHANNELS; i++) {
            pwm_output[i] = 1000;
        }
        if (_vehicle == ArduPlane) {
            pwm_output[0] = pwm_output[1] = pwm_output[3] = 1500;
        }
        if (_vehicle == APMrover2) {
            pwm_output[0] = pwm_output[1] = pwm_output[2] = pwm_output[3] = 1500;
        }
        for (i=0; i<SITL_NUM_CHANNELS; i++) {
            last_pwm_output[i] = pwm_output[i];
        }
    }

    // output at chosen framerate
    uint32_t now = AP_HAL::micros();
    float deltat = (now - last_update_usec) * 1.0e-6f;
    last_update_usec = now;

    _apply_servo_filter(deltat);

    // pass wind into simulators, using a wind gradient below 60m
    float altitude = _barometer?_barometer->get_altitude():0;
    float wind_speed = _sitl->wind_speed;
    if (altitude < 0) {
        altitude = 0;
    }
    if (altitude < 60) {
        wind_speed *= altitude / 60;
    }
    input.wind.speed = wind_speed;
    input.wind.direction = _sitl->wind_direction;
    input.wind.turbulence = _sitl->wind_turbulance;

    for (i=0; i<SITL_NUM_CHANNELS; i++) {
        if (pwm_output[i] == 0xFFFF) {
            input.servos[i] = 0;
        } else {
            input.servos[i] = pwm_output[i];
        }
        last_pwm_output[i] = pwm_output[i];
    }

    if (_vehicle == ArduPlane) {
        // add in engine multiplier
        if (input.servos[2] > 1000) {
            input.servos[2] = ((input.servos[2]-1000) * _sitl->engine_mul) + 1000;
            if (input.servos[2] > 2000) input.servos[2] = 2000;
        }
        _sitl->motors_on = ((input.servos[2]-1000)/1000.0f) > 0;
    } else if (_vehicle == APMrover2) {
        // add in engine multiplier
        if (input.servos[2] != 1500) {
            input.servos[2] = ((input.servos[2]-1500) * _sitl->engine_mul) + 1500;
            if (input.servos[2] > 2000) input.servos[2] = 2000;
            if (input.servos[2] < 1000) input.servos[2] = 1000;
        }
        _sitl->motors_on = ((input.servos[2]-1500)/500.0f) != 0;
    } else {
        _sitl->motors_on = false;
        // apply engine multiplier to first motor
        input.servos[0] = ((input.servos[0]-1000) * _sitl->engine_mul) + 1000;
        // run checks on each motor
        for (i=0; i<4; i++) {
            // check motors do not exceed their limits
            if (input.servos[i] > 2000) input.servos[i] = 2000;
            if (input.servos[i] < 1000) input.servos[i] = 1000;
            // update motor_on flag
            if ((input.servos[i]-1000)/1000.0f > 0) {
                _sitl->motors_on = true;
            }
        }
    }

    float voltage;
    
    if (_sitl->state.battery_voltage <= 0) {
        // simulate simple battery setup
        float throttle = _sitl->motors_on?(input.servos[2]-1000) / 1000.0f:0;
        // lose 0.7V at full throttle
        voltage = _sitl->batt_voltage - 0.7f*throttle;

        // assume 50A at full throttle
        _current = 50.0f * throttle;
    } else {
        // FDM provides voltage and current
        voltage = _sitl->state.battery_voltage;
        _current = _sitl->state.battery_current;
    }

    // assume 3DR power brick
    voltage_pin_value = ((voltage / 10.1f) / 5.0f) * 1024;
    current_pin_value = ((_current / 17.0f) / 5.0f) * 1024;
}


// generate a random float between -1 and 1
float SITL_State::_rand_float(void)
{
    return ((((unsigned)random()) % 2000000) - 1.0e6) / 1.0e6;
}

// generate a random Vector3f of size 1
Vector3f SITL_State::_rand_vec3f(void)
{
    Vector3f v = Vector3f(_rand_float(),
                          _rand_float(),
                          _rand_float());
    if (v.length() != 0.0f) {
        v.normalize();
    }
    return v;
}


void SITL_State::init(int argc, char * const argv[])
{
    pwm_input[0] = pwm_input[1] = pwm_input[3] = 1500;
    pwm_input[4] = pwm_input[7] = 1800;
    pwm_input[2] = pwm_input[5] = pwm_input[6] = 1000;

    _scheduler = SITLScheduler::from(hal.scheduler);
    _parse_command_line(argc, argv);
}

/*
  return height above the ground in meters
 */
float SITL_State::height_agl(void)
{
    static float home_alt = -1;

    if (home_alt == -1 && _sitl->state.altitude > 0) {
        // remember home altitude as first non-zero altitude
        home_alt = _sitl->state.altitude;
    }

    if (_terrain &&
            _sitl->terrain_enable) {
        // get height above terrain from AP_Terrain. This assumes
        // AP_Terrain is working
        float terrain_height_amsl;
        struct Location location;
        location.lat = _sitl->state.latitude*1.0e7;
        location.lng = _sitl->state.longitude*1.0e7;

        if (_terrain->height_amsl(location, terrain_height_amsl)) {
            return _sitl->state.altitude - terrain_height_amsl;
        }
    }

    // fall back to flat earth model
    return _sitl->state.altitude - home_alt;
}

#endif
