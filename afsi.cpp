#include <stddef.h>
#include <stdio.h>
#include "utils.h"
#include "mymath.h"
#include "HMC5883L.h"
#include "pGPS.h"
#include "IMU.h"
#include "defines.h"
#include "afsi.h"
#include "telemetry.h"
#include "pGPS.h"

extern FlightControlData hfc;
extern ConfigData *pConfig;
extern GPS gps;
extern XBus xbus;

AFSI_Serial::AFSI_Serial(RawSerial *m_serial, TelemSerial *m_telem)
{
    curr_msg.msg  = NULL;
    curr_msg.size = 0;
    num_msgs_in_q = 0;
    afsi_serial = m_serial;
    telem = m_telem;

    afsi_good_messages       = 0;
    afsi_crc_errors          = 0;
    afsi_msg_errors          = 0;
    afsi_msg_len_errors      = 0;
    afsi_start_code_searches = 0;

    afsi_rx_bytes  = 0;
    rx_payload_len = 0;
    rx_msg_state   = AFSI_STATE_INIT;
    rx_msg_rdy     = 0;
}

bool AFSI_Serial::QueueMsg(uint8_t *msg, int size, int msg_type)
{
    if (num_msgs_in_q >= MAX_AFSI_MSGS) {
        return false;
    }

    msg_queue[num_msgs_in_q].msg  = msg;
    msg_queue[num_msgs_in_q].size = size;
    msg_queue[num_msgs_in_q].msg_type = msg_type;
    num_msgs_in_q++;

    curr_msg = msg_queue[0];

    return true;
}

void AFSI_Serial::SendMsgs()
{
    int i;

    while(1) {

        /* if no more messages, exit */
        if (!num_msgs_in_q) {
            return;
        }

        /* if current message is in progress, keep sending it till serial buffer is full */
        while (curr_msg.size) {
            if (!afsi_serial->writeable()) {
                return;
            }
            afsi_serial->putc(*curr_msg.msg++);
            curr_msg.size--;
        }

        for ( i = 0; i < num_msgs_in_q; i++ ) {
            if( (i+1) == MAX_AFSI_MSGS ) {
                msg_queue[i].msg = NULL;
                msg_queue[i].msg_type = 0;
                msg_queue[i].size = 0;
            }
            else {
                msg_queue[i] = msg_queue[i+1];
            }
        }

        curr_msg = msg_queue[0];
        num_msgs_in_q--;
    }
}

bool AFSI_Serial::IsTypeInQ(int type)
{
    int i;

    if (curr_msg.msg_type == type) {
        return true;
    }

    for (i = 0; i < num_msgs_in_q; i++) {
        if (msg_queue[i].msg_type == type) {
            return true;
        }
    }

    return false;
}

void AFSI_Serial::ProcessInputBytes(RawSerial &afsi_serial)
{
    while (afsi_serial.readable()) {
        AddInputByte(afsi_serial.getc());
    }
}

void AFSI_Serial::AddInputByte(uint8_t rx_byte)
{
    int len = 0;

    if (afsi_rx_bytes > AFSI_BUFFER_SIZE) {
        printf("ERROR: Message Discarded!\r\n");
        ResetRxMsgData();
    }

    debug_print("Byte read: %d\r\n", rx_byte);

    switch (rx_msg_state)
    {
        case AFSI_STATE_INIT:
            if(rx_byte == AFSI_SYNC_BYTE_1) {
                afsi_rx_buffer[afsi_rx_bytes] = rx_byte;
                afsi_rx_bytes++;
                rx_msg_state = AFSI_STATE_SYNC;
                debug_print("AFSI_SYNC_BYTE_1 has been FOUND!\r\n");
            }
            else {
                ResetRxMsgData();
            }
            break;
        case AFSI_STATE_SYNC:
            if(rx_byte == AFSI_SYNC_BYTE_2) {
                afsi_rx_buffer[afsi_rx_bytes] = rx_byte;
                afsi_rx_bytes++;
                rx_msg_state = AFSI_STATE_CLASS_ID;
                debug_print("AFSI_SYNC_BYTE_2 has been FOUND!\r\n");
            }
            else {
                ResetRxMsgData();
            }
            break;
        case AFSI_STATE_CLASS_ID:
            afsi_rx_buffer[afsi_rx_bytes] = rx_byte;
            afsi_rx_bytes++;
            if( afsi_rx_bytes >= 4 ) {
                rx_msg_state = AFSI_STATE_LEN;
                debug_print("AFSI CLASS ID byte has been FOUND!\r\n");
            }
            break;
        case AFSI_STATE_LEN:
            afsi_rx_buffer[afsi_rx_bytes] = rx_byte;
            afsi_rx_bytes++;
            if( afsi_rx_bytes >= 6 ) {
                rx_msg_state = AFSI_STATE_READ;
                rx_payload_len = ((uint16_t)afsi_rx_buffer[5] << 8 ) + (uint16_t)afsi_rx_buffer[4];
                debug_print("AFSI data length = %d\r\n", rx_payload_len);
            }
            break;
        case AFSI_STATE_READ:
            afsi_rx_buffer[afsi_rx_bytes] = rx_byte;
            afsi_rx_bytes++;
            if( afsi_rx_bytes >= (AFSI_HEADER_LEN + rx_payload_len + AFSI_CRC_LEN) ) {
                rx_msg_rdy = 1;
                rx_msg_state = AFSI_STATE_INIT;
                debug_print("AFSI PAYLOAD and CRC data has been FOUND!\r\n");
            }
            break;
        default:
            break;
    }


    if (rx_msg_rdy == 1) {

        debug_print("\r\nRAW DATA HEX: ");
        for (int i = 0; i < (int)afsi_rx_bytes; i++) {
            debug_print("%02x ", afsi_rx_buffer[i]);
        }
        debug_print("\r\nRAW DATA DEC: ");
        for (int i = 0; i < (int)afsi_rx_bytes; i++) {
            debug_print("%d ", afsi_rx_buffer[i]);
        }
        debug_print("\r\n");

        memcpy(&msg_afsi, afsi_rx_buffer, afsi_rx_bytes);
        len = AFSI_HEADER_LEN - AFSI_SYCN_LEN + rx_payload_len;
        //Get, set, and check CRC
        if ( !GetCRC(&(afsi_rx_buffer[2]), len, &(msg_afsi.crc.crc[0])) ) {
            afsi_crc_errors++;
            debug_print("ERROR: AFSI CRC ERRORS = %d\r\n",afsi_crc_errors);
            debug_print("       CRC Calc = %d %d\r\n", msg_afsi.crc.crc[0], msg_afsi.crc.crc[1]);
            debug_print("       CRC Read = %d %d\r\n", afsi_rx_buffer[len+2], afsi_rx_buffer[len+3]);
            ResetRxMsgData();
            return;
        }

        if ( msg_afsi.hdr.msg_class == AFSI_CTRL ) {
            if (ProcessAsfiCtrlCommands(&msg_afsi)) {
                afsi_good_messages++;
                GenerateACK(msg_afsi.hdr.msg_class,msg_afsi.hdr.id);
            }
            else {
                afsi_msg_errors++;
                GenerateNACK(msg_afsi.hdr.msg_class,msg_afsi.hdr.id);
                debug_print("ERROR: AFSI MSG ERRORS = %d\r\n",afsi_msg_errors);
            }
        }
        else if ( msg_afsi.hdr.msg_class == AFSI_STATUS ) {
            if (ProcessAsfiStatusCommands(&msg_afsi)){
                afsi_good_messages++;
            }
            else {
                afsi_msg_errors++;
                GenerateNACK(msg_afsi.hdr.msg_class,msg_afsi.hdr.id);
                debug_print("ERROR: AFSI MSG ERRORS = %d\r\n",afsi_msg_errors);
            }
        }

        ResetRxMsgData();
    }

    return;

}

void AFSI_Serial::ResetRxMsgData(void)
{
    rx_payload_len = 0;
    rx_msg_rdy = 0;
    afsi_rx_bytes = 0;
    rx_msg_state = AFSI_STATE_INIT;
    return;
}

int AFSI_Serial::GetCRC(uint8_t *data, int len, uint8_t *CRC)
{
    int i;
    uint8_t CK_A = 0;
    uint8_t CK_B = 0;

    for (i=0; i<len; i++) {
        CK_A = CK_A + data[i];
        CK_B = CK_B + CK_A;
    }

    CRC[0] = CK_A;
    CRC[1] = CK_B;

    if ( (CK_A != data[len+0]) || (CK_B != data[len+1]) ) {
        return 0;
    }

    return 1;
}

void AFSI_Serial::EnableAFSI(void) {

//    ctrl_out[AFSI_SPEED_FWD]   = 0;
//    ctrl_out[AFSI_SPEED_RIGHT] = 0;
//    ctrl_out[AFSI_ALTITUDE]    = 0;
//    ctrl_out[AFSI_HEADING]     = 0;

    /* altitude hold and yaw angle */
    SetCtrlMode(&hfc, pConfig, PITCH, CTRL_MODE_SPEED);
    SetCtrlMode(&hfc, pConfig, ROLL,  CTRL_MODE_SPEED);
    SetCtrlMode(&hfc, pConfig, YAW,   CTRL_MODE_ANGLE);
    SetCtrlMode(&hfc, pConfig, COLL,  CTRL_MODE_POSITION);
    /* set target altitude and heading to the current one */
    hfc.ctrl_out[ANGLE][YAW] = hfc.IMUorient[YAW]*R2D;

    if (hfc.playlist_status==PLAYLIST_PLAYING)
    {
        telem->PlaylistSaveState();
        telem->SelectCtrlSource(CTRL_SOURCE_AFSI);
        hfc.playlist_status = PLAYLIST_PAUSED;
    }
    else
    if (hfc.playlist_status==PLAYLIST_PAUSED)
    {
        telem->SelectCtrlSource(CTRL_SOURCE_AFSI);
        hfc.playlist_status = PLAYLIST_PAUSED;
    }
    else {
        telem->SelectCtrlSource(CTRL_SOURCE_AFSI);
    }

    return;
}

int AFSI_Serial::ProcessAsfiCtrlCommands(AFSI_MSG *msg)
{
    float lat, lon, speed, alt, heading = {0.0f};

    EnableAFSI();

    if (rx_payload_len != ctrl_msg_lengths[msg->hdr.id]) {
        afsi_msg_len_errors++;
        debug_print("ERROR: AFSI MSG LEN ERRORS = %d\r\n",afsi_msg_len_errors);
        debug_print("       Max pay load length = %d\r\n",ctrl_msg_lengths[msg->hdr.id]);
        debug_print("       Pay load length     = %d\r\n",rx_payload_len);
        return 0;
    }

    switch (msg->hdr.id){
        case AFSI_CTRL_ID_ARM:
            telem->Arm();
            debug_print("ARMED!\r\n");
            break;

        case AFSI_CTRL_ID_DISARM:
            telem->Disarm();
            debug_print("DISARMED!\r\n");
            break;

        case AFSI_CTRL_ID_TAKEOFF:
            alt = ProcessUint16((uint16_t*)msg->payload,AFSI_SCALE_ALT);
            debug_print("alt = %f\r\n",alt);

            if (CheckRangeF(alt, AFSI_MIN_ALT, AFSI_MAX_ALT)) {
                hfc.home_pos[2] = alt;
                hfc.afsi_enable = 1;
                telem->CommandTakeoffArm();
            }
            else {
                return 0;
            }
            break;

        case  AFSI_CTRL_ID_LAND:
            hfc.playlist_status = PLAYLIST_STOP;
            // TODO MMRI: check if this is immediate landing
            telem->CommandLanding(false, true);
            debug_print("LANDING!\r\n");
            break;

        case AFSI_CTRL_ID_SET_POS:
            // the AFSI Position command payload is padded with an extra
            // two dummy bytes to make storage of the message struct easier
            // Latitude and Longitude data starts at msg.payload[2]
            lat = ProcessInt32((int32_t*)(&msg->payload[2]),AFSI_SCALE_POS);
            lon = ProcessInt32((int32_t*)(&msg->payload[6]),AFSI_SCALE_POS);

            debug_print("lat = %f\r\n",lat);
            debug_print("lon = %f\r\n",lon);

            if ( CheckRangeF(lat, -90, 90)   ||
                 CheckRangeF(lon, -180, 180)    ) {

                /* since this is asynchronous to the main loop, the control_mode
                 * change might be detected and the init skipped,
                 * thus it needs to be done here */
                if (hfc.control_mode[PITCH] < CTRL_MODE_POSITION) {
                    PID_SetForEnable(&hfc.pid_Dist2T, 0, 0, hfc.gps_speed);
                    PID_SetForEnable(&hfc.pid_Dist2P, 0, 0, 0);
                    hfc.speedCtrlPrevEN[0] = 0;
                    hfc.speedCtrlPrevEN[1] = 0;
                }

                telem->SetWaypoint(lat, lon, -9999, WAYPOINT_GOTO, false);
                return 1;
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_SPEED_FWD:
            speed = ProcessInt16((int16_t*)msg->payload,AFSI_SCALE_SPEED);
            debug_print("speed_fwd = %f\r\n",speed);

            if (CheckRangeF(speed, AFSI_MIN_SPEED, AFSI_MAX_SPEED)) {
                ctrl_out[AFSI_SPEED_FWD] = speed;
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_SPEED_RIGHT:
            speed = ProcessInt16((int16_t*)msg->payload,AFSI_SCALE_SPEED);
            debug_print("speed_right = %f\r\n",speed);

            if (CheckRangeF(speed, AFSI_MIN_SPEED, AFSI_MAX_SPEED)) {
                ctrl_out[AFSI_SPEED_RIGHT] = speed;
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_SET_ALT:
            alt = ProcessUint16((uint16_t*)msg->payload,AFSI_SCALE_ALT);
            debug_print("alt = %f\r\n",alt);

            if (CheckRangeF(alt, AFSI_MIN_ALT, AFSI_MAX_ALT)) {
                ctrl_out[AFSI_ALTITUDE] = alt;
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_HOME:
            speed = ProcessUint16((uint16_t*)msg->payload,AFSI_SCALE_SPEED);
            debug_print("speed_to_home = %f\r\n",speed);

            if (CheckRangeF(speed, AFSI_MIN_SPEED, AFSI_MAX_SPEED) && (speed > 0) ) {
                hfc.pid_Dist2T.COmax = speed;
                telem->ApplyDefaults();
                telem->SetWaypoint(hfc.home_pos[0], hfc.home_pos[1], -9999, WAYPOINT_GOTO, 0);
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_HOLD:
            telem->SetPositionHold();
            debug_print("HOLD POSITION!\r\n");
            break;

        case AFSI_CTRL_ID_HEADING:
            heading = ProcessInt16((int16_t*)msg->payload,AFSI_SCALE_HEADING);
            debug_print("heading = %f\r\n",heading);

            if (CheckRangeF(heading, AFSI_MIN_HEADING, AFSI_MAX_HEADING)) {
                ctrl_out[AFSI_HEADING] = heading;
            }
            else {
                return 0;
            }
            break;

        case AFSI_CTRL_ID_RESUME:
            if (hfc.playlist_status == PLAYLIST_PAUSED) {
                telem->PlaylistRestoreState();
            }

            debug_print("RESUME PLAYLIST!\r\n");
            break;

        default:
            return 0;
    }

    return 1;
}

int AFSI_Serial::ProcessAsfiStatusCommands(AFSI_MSG *msg)
{
    if (rx_payload_len != AFSI_RX_STAT_PAYL_LEN) {
        return 0;
    }

    int period = msg->payload[0];

    if( msg->hdr.id < AFSI_NUM_STAT_MSGS) {
        stat_msg_enable[msg->hdr.id] = 1;
        stat_msg_cnt[msg->hdr.id]    = 0;
        stat_msg_period[msg->hdr.id] = period;
    }
    else {
        return 0;
    }

    return 1;
}

void AFSI_Serial::GenerateStatMsg(int id)
{
    switch (id) {
        case AFSI_STAT_ID_PWR:
            GeneratePwrStatus();
            break;
        case AFSI_STAT_ID_GPS:
            GenerateGpsStatus();
            break;
        case AFSI_STAT_ID_SEN:
            GenerateSensorsStatus();
            break;
        case AFSI_STAT_ID_FCM:
            GenerateFcmStatus();
            break;
        default:
            break;
    }
}

void AFSI_Serial::GeneratePwrStatus(void) {

    msg_stat_pwr.bat_capacity_used  = (hfc.power.capacity_used/3600.0f) * AFSI_STAT_SCALE_PWR;
    msg_stat_pwr.total_bat_capacity = (hfc.power.capacity_total/3600.0f)* AFSI_STAT_SCALE_PWR;
    msg_stat_pwr.bat_percent_used   = hfc.power.battery_level  * AFSI_STAT_SCALE_PWR;
    msg_stat_pwr.current            = hfc.power.Iesc           * AFSI_STAT_SCALE_PWR;
    msg_stat_pwr.voltage            = hfc.power.Vmain          * AFSI_STAT_SCALE_PWR;
    msg_stat_pwr.flight_time        = hfc.power.flight_time_left;

    GetCRC( (uint8_t*)&(msg_stat_pwr.msg_class),
            msg_stat_pwr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_stat_pwr.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_stat_pwr),
               msg_stat_pwr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_PWR );
    return;

}

void AFSI_Serial::GenerateGpsStatus(void)
{
    msg_stat_gps.hdr.sync1     = AFSI_SYNC_BYTE_1;
    msg_stat_gps.hdr.sync2     = AFSI_SYNC_BYTE_2;
    msg_stat_gps.hdr.msg_class = AFSI_CMD_CLASS_STAT;
    msg_stat_gps.hdr.id        = AFSI_STAT_ID_GPS;
    msg_stat_gps.hdr.len       = AFSI_STAT_PAYL_LEN_GPS;
    msg_stat_gps.hour          = gps.gps_data_.time/1000000;
    msg_stat_gps.min           = (gps.gps_data_.time-(msg_stat_gps.hour*1000000))/10000;
    msg_stat_gps.sec           = (gps.gps_data_.time-(msg_stat_gps.hour*1000000)-(msg_stat_gps.min*10000))/100;
    msg_stat_gps.year          = gps.gps_data_.date/10000;
    msg_stat_gps.month         = (gps.gps_data_.date-(msg_stat_gps.year*10000)) / 100;
    msg_stat_gps.day           = gps.gps_data_.date-(msg_stat_gps.year*10000)-(msg_stat_gps.month*100);
    msg_stat_gps.altitude      = gps.gps_data_.altitude * AFSI_STAT_SCALE_GPS_ALT;
    msg_stat_gps.latitude      = gps.gps_data_.lat;  // this variable is already scaled by AFSI_STAT_SCALE_POS;
    msg_stat_gps.longitude     = gps.gps_data_.lon;  // this variable is already scaled by AFSI_STAT_SCALE_POS;
    msg_stat_gps.pDOP          = gps.gps_data_.PDOP     * AFSI_STAT_SCALE_PDOP;
    msg_stat_gps.numSV         = gps.gps_data_.sats;

    GetCRC( (uint8_t*)&(msg_stat_gps.hdr.msg_class),
            msg_stat_gps.hdr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_stat_gps.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_stat_gps),
               msg_stat_gps.hdr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_GPS );

    return;
}

void AFSI_Serial::GenerateSensorsStatus(void)
{
    msg_stat_sensors.hdr.sync1       = AFSI_SYNC_BYTE_1;
    msg_stat_sensors.hdr.sync2       = AFSI_SYNC_BYTE_2;
    msg_stat_sensors.hdr.msg_class   = AFSI_CMD_CLASS_STAT;
    msg_stat_sensors.hdr.id          = AFSI_STAT_ID_SEN;
    msg_stat_sensors.hdr.len         = AFSI_STAT_PAYL_LEN_SEN;
    msg_stat_sensors.gyro_temp       = hfc.gyro_temp_lp       * AFSI_STAT_SCALE_TEMP;
    msg_stat_sensors.baro_temp       = hfc.baro_temperature   * AFSI_STAT_SCALE_TEMP;
    msg_stat_sensors.esc_temp        = hfc.esc_temp           * AFSI_STAT_SCALE_TEMP;
    msg_stat_sensors.baro_pressure   = hfc.baro_pressure;
    msg_stat_sensors.wind_speed      = hfc.wind_speed         * AFSI_STAT_SCALE_WIND;
    msg_stat_sensors.wind_course     = hfc.wind_course        * AFSI_STAT_SCALE_WIND;
    msg_stat_sensors.rpm             = hfc.RPM;
    msg_stat_sensors.lidar_altitude  = hfc.altitude_lidar     * AFSI_STAT_SCALE_LIDAR;
    msg_stat_sensors.compass_heading = hfc.compass_heading_lp * AFSI_STAT_SCALE_COMPASS;
    msg_stat_sensors.altitude        = hfc.altitude           * AFSI_STAT_SCALE_ALT;

    GetCRC( (uint8_t*)&(msg_stat_sensors.hdr.msg_class),
            msg_stat_sensors.hdr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_stat_sensors.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_stat_sensors),
               msg_stat_sensors.hdr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_SEN );

    return;
}

void AFSI_Serial::GenerateFcmStatus(void)
{
    uint8_t waypoint_ctrl_mode = ( (hfc.ctrl_source == CTRL_SOURCE_AUTO2D) ||
                                   (hfc.ctrl_source == CTRL_SOURCE_AUTO3D)    ) ? 1 : 0;

    uint8_t joystick_ctrl_mode =   (hfc.ctrl_source == CTRL_SOURCE_JOYSTICK) ? 1 : 0;

    msg_stat_fcm.hdr.sync1            = AFSI_SYNC_BYTE_1;
    msg_stat_fcm.hdr.sync2            = AFSI_SYNC_BYTE_2;
    msg_stat_fcm.hdr.msg_class        = AFSI_CMD_CLASS_STAT;
    msg_stat_fcm.hdr.id               = AFSI_STAT_ID_FCM;
    msg_stat_fcm.hdr.len              = AFSI_STAT_PAYL_LEN_FCM;
    msg_stat_fcm.ctrl_mode_pitch      = hfc.control_mode[PITCH];
    msg_stat_fcm.ctrl_mode_roll       = hfc.control_mode[ROLL];
    msg_stat_fcm.ctrl_mode_yaw        = hfc.control_mode[YAW];
    msg_stat_fcm.ctrl_mode_collective = hfc.control_mode[COLL];
    msg_stat_fcm.ctrl_mode_throttle   = hfc.control_mode[THRO];

    msg_stat_fcm.ctrl_status   =   (          xbus.receiving & 1          ) |
                                   (          (waypoint_ctrl_mode)   << 1 ) |
                                   ( ( (!hfc.throttle_armed) & 1   ) << 2 ) |
                                   (     (joystick_ctrl_mode & 1   ) << 3 ) |
                                   (   ( hfc.playlist_status & 0x3 ) << 4 ) |
                                   (         ( hfc.full_auto & 1   ) << 6 );

    GetCRC( (uint8_t*)&(msg_stat_fcm.hdr.msg_class),
            msg_stat_fcm.hdr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_stat_fcm.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_stat_fcm),
               msg_stat_fcm.hdr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_FCM );

    return;
}

void AFSI_Serial::GenerateACK(int msg_class, int msg_id)
{
    msg_ack.hdr.sync1      = AFSI_SYNC_BYTE_1;
    msg_ack.hdr.sync2      = AFSI_SYNC_BYTE_2;
    msg_ack.hdr.msg_class  = AFSI_CMD_CLASS_ACK;
    msg_ack.hdr.id         = AFSI_ACK_ID_ACK;
    msg_ack.hdr.len        = AFSI_ACK_PAYL_LEN;
    msg_ack.ackd_msg_class = msg_class;
    msg_ack.ackd_msg_id    = msg_id;

    GetCRC( (uint8_t*)&(msg_ack.hdr.msg_class),
            msg_ack.hdr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_ack.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_ack),
               msg_ack.hdr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_ACK );

    return;
}

void AFSI_Serial::GenerateNACK(int msg_class, int msg_id)
{
    msg_nack.hdr.sync1       = AFSI_SYNC_BYTE_1;
    msg_nack.hdr.sync2       = AFSI_SYNC_BYTE_2;
    msg_nack.hdr.msg_class   = AFSI_CMD_CLASS_ACK;
    msg_nack.hdr.id          = AFSI_ACK_ID_NACK;
    msg_nack.hdr.len         = AFSI_ACK_PAYL_LEN;
    msg_nack.nackd_msg_class = msg_class;
    msg_nack.nackd_msg_id    = msg_id;

    GetCRC( (uint8_t*)&(msg_nack.hdr.msg_class),
            msg_nack.hdr.len + AFSI_HEADER_LEN - AFSI_SYCN_LEN,
            &(msg_nack.crc.crc[0]) );

    QueueMsg( (uint8_t*)&(msg_nack),
               msg_nack.hdr.len + AFSI_HEADER_LEN + AFSI_CRC_LEN,
               AFSI_TX_TYPE_NACK );

    return;
}

bool AFSI_Serial::CheckRangeF(float value, float min, float max)
{
    if ( (value > max) || (value < min) ) {
        return false;
    }

    return true;
}

float AFSI_Serial::ProcessUint16(uint16_t*data, float scaling)
{
    float result = 0;
    result = data[0]*scaling;

    return result;
}

float AFSI_Serial::ProcessInt16(int16_t*data, float scaling)
{
    float result = 0;
    result = data[0]*scaling;

    return result;
}

float AFSI_Serial::ProcessInt32(int32_t*data, float scaling)
{
    float result = 0;
    result = data[0]*scaling;

    return result;
}
