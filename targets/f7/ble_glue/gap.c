#include "gap.h"

#include "app_common.h"
#include <core/mutex.h>
#include "furi_ble/event_dispatcher.h"
#include <ble/ble.h>

#include <furi_hal.h>
#include <furi.h>
#include <stdint.h>

#define TAG "BleGap"

#define FAST_ADV_TIMEOUT    30000
#define INITIAL_ADV_TIMEOUT 60000

#define GAP_INTERVAL_TO_MS(x) (uint16_t)((x) * 1.25)

typedef struct {
    uint16_t gap_svc_handle;
    uint16_t dev_name_char_handle;
    uint16_t appearance_char_handle;
    uint16_t connection_handle;
    uint8_t adv_svc_uuid_len;
    uint8_t adv_svc_uuid[20];
    uint8_t mfg_data_len;
    uint8_t mfg_data[23];
    char* adv_name;
} GapSvc;

typedef struct {
    GapSvc service;
    GapConfig* config;
    GapConnectionParams connection_params;
    GapState state;
    FuriMutex* state_mutex;
    GapEventCallback on_event_cb;
    void* context;
    FuriTimer* advertise_timer;
    FuriThread* thread;
    FuriMessageQueue* command_queue;
    bool enable_adv;
    bool is_secure;
    uint8_t negotiation_round;
} Gap;

typedef enum {
    GapCommandAdvFast,
    GapCommandAdvLowPower,
    GapCommandAdvStop,
    GapCommandKillThread,
} GapCommand;

// Identity root key
static const uint8_t gap_irk[16] =
    {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
// Encryption root key
static const uint8_t gap_erk[16] =
    {0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x43, 0x21, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x43, 0x21};

static Gap* gap = NULL;

static void gap_advertise_start(GapState new_state);
static int32_t gap_app(void* context);

static void gap_verify_connection_parameters(Gap* gap) {
    furi_check(gap);

    FURI_LOG_I(
        TAG,
        "Connection parameters: Connection Interval: %d (%d ms), Slave Latency: %d, Supervision Timeout: %d",
        gap->connection_params.conn_interval,
        GAP_INTERVAL_TO_MS(gap->connection_params.conn_interval),
        gap->connection_params.slave_latency,
        gap->connection_params.supervisor_timeout);

    // Send connection parameters request update if necessary
    GapConnectionParamsRequest* params = &gap->config->conn_param;

    // Desired max connection interval depends on how many negotiation rounds we had in the past
    // In the first negotiation round we want connection interval to be minimum
    // If platform disagree then we request wider range
    uint16_t connection_interval_max = gap->negotiation_round ? params->conn_int_max :
                                                                params->conn_int_min;

    // We do care about lower connection interval bound a lot: if it's lower than 30ms 2nd core will not allow us to use flash controller
    bool negotiation_failed = params->conn_int_min > gap->connection_params.conn_interval;

    // We don't care about upper bound till connection become secure
    if(gap->is_secure) {
        negotiation_failed |= connection_interval_max < gap->connection_params.conn_interval;
    }

    if(negotiation_failed) {
        FURI_LOG_W(
            TAG,
            "Connection interval doesn't suite us. Trying to negotiate, round %u",
            gap->negotiation_round + 1);
        if(aci_l2cap_connection_parameter_update_req(
               gap->service.connection_handle,
               params->conn_int_min,
               connection_interval_max,
               gap->connection_params.slave_latency,
               gap->connection_params.supervisor_timeout)) {
            FURI_LOG_E(TAG, "Failed to request connection parameters update");
            // The other side is not in the mood
            // But we are open to try it again
            gap->negotiation_round = 0;
        } else {
            gap->negotiation_round++;
        }
    } else {
        FURI_LOG_I(
            TAG,
            "Connection interval suits us. Spent %u rounds to negotiate",
            gap->negotiation_round);
        // Looks like the other side is open to negotiation
        gap->negotiation_round = 0;
    }
}

BleEventFlowStatus ble_event_app_notification(void* pckt) {
    hci_event_pckt* event_pckt;
    evt_le_meta_event* meta_evt;
    evt_blecore_aci* blue_evt;
    hci_le_phy_update_complete_event_rp0* evt_le_phy_update_complete;
    uint8_t tx_phy;
    uint8_t rx_phy;
    tBleStatus ret = BLE_STATUS_INVALID_PARAMS;

    event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;

    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    switch(event_pckt->evt) {
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        hci_disconnection_complete_event_rp0* disconnection_complete_event =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;
        if(disconnection_complete_event->Connection_Handle == gap->service.connection_handle) {
            gap->service.connection_handle = 0;
            gap->state = GapStateIdle;
            FURI_LOG_I(
                TAG, "Disconnect from client. Reason: %02X", disconnection_complete_event->Reason);
        }
        gap->is_secure = false;
        gap->negotiation_round = 0;
        // Enterprise sleep
        furi_delay_us(666 + 666);
        if(gap->enable_adv) {
            // Restart advertising
            gap_advertise_start(GapStateAdvFast);
        }
        GapEvent event = {.type = GapEventTypeDisconnected};
        gap->on_event_cb(event, gap->context);
    } break;

    case HCI_LE_META_EVT_CODE:
        meta_evt = (evt_le_meta_event*)event_pckt->data;
        switch(meta_evt->subevent) {
        case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_update_complete_event_rp0* event =
                (hci_le_connection_update_complete_event_rp0*)meta_evt->data;
            gap->connection_params.conn_interval = event->Conn_Interval;
            gap->connection_params.slave_latency = event->Conn_Latency;
            gap->connection_params.supervisor_timeout = event->Supervision_Timeout;
            FURI_LOG_I(TAG, "Connection parameters event complete");
            gap_verify_connection_parameters(gap);
            break;
        }

        case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE:
            evt_le_phy_update_complete = (hci_le_phy_update_complete_event_rp0*)meta_evt->data;
            if(evt_le_phy_update_complete->Status) {
                FURI_LOG_E(
                    TAG, "Update PHY failed, status %d", evt_le_phy_update_complete->Status);
            } else {
                FURI_LOG_I(TAG, "Update PHY succeed");
            }
            ret = hci_le_read_phy(gap->service.connection_handle, &tx_phy, &rx_phy);
            if(ret) {
                FURI_LOG_E(TAG, "Read PHY failed, status: %d", ret);
            } else {
                FURI_LOG_I(TAG, "PHY Params TX = %d, RX = %d ", tx_phy, rx_phy);
            }
            break;

        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_complete_event_rp0* event =
                (hci_le_connection_complete_event_rp0*)meta_evt->data;
            gap->connection_params.conn_interval = event->Conn_Interval;
            gap->connection_params.slave_latency = event->Conn_Latency;
            gap->connection_params.supervisor_timeout = event->Supervision_Timeout;

            // Stop advertising as connection completed
            furi_timer_stop(gap->advertise_timer);

            // Update connection status and handle
            gap->state = GapStateConnected;
            gap->service.connection_handle = event->Connection_Handle;

            gap_verify_connection_parameters(gap);
            if(gap->config->pairing_method != GapPairingNone) {
                // Start pairing by sending security request
                aci_gap_slave_security_req(event->Connection_Handle);
            }
        } break;

        default:
            break;
        }
        break;

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE:
        blue_evt = (evt_blecore_aci*)event_pckt->data;
        switch(blue_evt->ecode) {
            aci_gap_pairing_complete_event_rp0* pairing_complete;

        case ACI_GAP_LIMITED_DISCOVERABLE_VSEVT_CODE:
            FURI_LOG_I(TAG, "Limited discoverable event");
            break;

        case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE: {
            // Generate random PIN code
            uint32_t pin = rand() % 999999; //-V1064
            aci_gap_pass_key_resp(gap->service.connection_handle, pin);
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
                FURI_LOG_I(TAG, "Pass key request event. Pin: ******");
            } else {
                FURI_LOG_I(TAG, "Pass key request event. Pin: %06ld", pin);
            }
            GapEvent event = {.type = GapEventTypePinCodeShow, .data.pin_code = pin};
            gap->on_event_cb(event, gap->context);
        } break;

        case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
            aci_att_exchange_mtu_resp_event_rp0* pr = (void*)blue_evt->data;
            FURI_LOG_I(TAG, "Rx MTU size: %d", pr->Server_RX_MTU);
            // Set maximum packet size given header size is 3 bytes
            GapEvent event = {
                .type = GapEventTypeUpdateMTU, .data.max_packet_size = pr->Server_RX_MTU - 3};
            gap->on_event_cb(event, gap->context);
        } break;

        case ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE:
            FURI_LOG_D(TAG, "Authorization request event");
            break;

        case ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE:
            FURI_LOG_D(TAG, "Slave security initiated");
            gap->is_secure = true;
            break;

        case ACI_GAP_BOND_LOST_VSEVT_CODE:
            FURI_LOG_D(TAG, "Bond lost event. Start rebonding");
            aci_gap_allow_rebond(gap->service.connection_handle);
            break;

        case ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE:
            FURI_LOG_D(TAG, "Address not resolved event");
            break;

        case ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE:
            FURI_LOG_D(TAG, "Key press notification event");
            break;

        case ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE: {
            uint32_t pin =
                ((aci_gap_numeric_comparison_value_event_rp0*)(blue_evt->data))->Numeric_Value;
            FURI_LOG_I(TAG, "Verify numeric comparison: %06lu", pin);
            GapEvent event = {.type = GapEventTypePinCodeVerify, .data.pin_code = pin};
            bool result = gap->on_event_cb(event, gap->context);
            aci_gap_numeric_comparison_value_confirm_yesno(gap->service.connection_handle, result);
            break;
        }

        case ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE:
            pairing_complete = (aci_gap_pairing_complete_event_rp0*)blue_evt->data;
            if(pairing_complete->Status) {
                FURI_LOG_E(
                    TAG,
                    "Pairing failed with status: %d. Terminating connection",
                    pairing_complete->Status);
                aci_gap_terminate(gap->service.connection_handle, 5);
            } else {
                FURI_LOG_I(TAG, "Pairing complete");
                GapEvent event = {.type = GapEventTypeConnected};
                gap->on_event_cb(event, gap->context); //-V595
            }
            break;

        case ACI_L2CAP_CONNECTION_UPDATE_RESP_VSEVT_CODE:
            FURI_LOG_D(TAG, "Procedure complete event");
            break;

        case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE: {
            uint16_t result =
                ((aci_l2cap_connection_update_resp_event_rp0*)(blue_evt->data))->Result;
            if(result == 0) {
                FURI_LOG_D(TAG, "Connection parameters accepted");
            } else if(result == 1) {
                FURI_LOG_D(TAG, "Connection parameters denied");
            }
            break;
        }
        }
    default:
        break;
    }

    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);

    return BleEventFlowEnable;
}

static void set_advertisment_service_uid(uint8_t* uid, uint8_t uid_len) {
    if(uid_len == 2) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_16_BIT_SERV_UUID;
    } else if(uid_len == 4) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_32_BIT_SERV_UUID;
    } else if(uid_len == 16) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_128_BIT_SERV_UUID_CMPLT_LIST;
    }
    memcpy(&gap->service.adv_svc_uuid[gap->service.adv_svc_uuid_len], uid, uid_len);
    gap->service.adv_svc_uuid_len += uid_len;
}

static void set_manufacturer_data(uint8_t* mfg_data, uint8_t mfg_data_len) {
    furi_check(mfg_data_len <= sizeof(gap->service.mfg_data) - 2);
    gap->service.mfg_data[0] = mfg_data_len + 1;
    gap->service.mfg_data[1] = AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
    memcpy(&gap->service.mfg_data[gap->service.mfg_data_len], mfg_data, mfg_data_len);
    gap->service.mfg_data_len += mfg_data_len;
}

static void gap_init_svc(Gap* gap) {
    tBleStatus status;
    uint32_t srd_bd_addr[2];

    // Configure mac address
    aci_hal_write_config_data(
        CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, gap->config->mac_address);

    /* Static random Address
     * The two upper bits shall be set to 1
     * The lowest 32bits is read from the UDN to differentiate between devices
     * The RNG may be used to provide a random number on each power on
     */
    srd_bd_addr[1] = 0x0000ED6E;
    srd_bd_addr[0] = LL_FLASH_GetUDN();
    aci_hal_write_config_data(
        CONFIG_DATA_RANDOM_ADDRESS_OFFSET, CONFIG_DATA_RANDOM_ADDRESS_LEN, (uint8_t*)srd_bd_addr);
    // Set Identity root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_IR_OFFSET, CONFIG_DATA_IR_LEN, (uint8_t*)gap_irk);
    // Set Encryption root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_ER_OFFSET, CONFIG_DATA_ER_LEN, (uint8_t*)gap_erk);
    // Set TX Power to 0 dBm
    aci_hal_set_tx_power_level(1, 0x19);
    // Initialize GATT interface
    aci_gatt_init();
    // Initialize GAP interface
    // Skip fist symbol AD_TYPE_COMPLETE_LOCAL_NAME
    char* name = gap->service.adv_name + 1;
    aci_gap_init(
        GAP_PERIPHERAL_ROLE,
        0,
        strlen(name),
        &gap->service.gap_svc_handle,
        &gap->service.dev_name_char_handle,
        &gap->service.appearance_char_handle);

    // Set GAP characteristics
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.dev_name_char_handle,
        0,
        strlen(name),
        (uint8_t*)name);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating name characteristic: %d", status);
    }

    uint8_t gap_appearence_char_uuid[2] = {
        gap->config->appearance_char & 0xff, gap->config->appearance_char >> 8};
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.appearance_char_handle,
        0,
        2,
        gap_appearence_char_uuid);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating appearence characteristic: %d", status);
    }
    // Set default PHY
    hci_le_set_default_phy(ALL_PHYS_PREFERENCE, TX_2M_PREFERRED, RX_2M_PREFERRED);
    // Set I/O capability
    uint8_t auth_req_mitm_mode = MITM_PROTECTION_REQUIRED;
    uint8_t auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_FORBIDDEN;
    bool keypress_supported = false;
    if(gap->config->pairing_method == GapPairingPinCodeShow) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_ONLY);
    } else if(gap->config->pairing_method == GapPairingPinCodeVerifyYesNo) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
        keypress_supported = true;
    } else if(gap->config->pairing_method == GapPairingNone) {
        // "Just works" pairing method (iOS accepts it, it seems Android and Linux don't)
        auth_req_mitm_mode = MITM_PROTECTION_NOT_REQUIRED;
        auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_ALLOWED;
        // If "just works" isn't supported, we want the numeric comparaison method
        aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
        keypress_supported = true;
    }
    // Setup  authentication
    aci_gap_set_authentication_requirement(
        gap->config->bonding_mode,
        auth_req_mitm_mode,
        CFG_SC_SUPPORT,
        keypress_supported,
        CFG_ENCRYPTION_KEY_SIZE_MIN,
        CFG_ENCRYPTION_KEY_SIZE_MAX,
        auth_req_use_fixed_pin,
        0,
        CFG_IDENTITY_ADDRESS);
    // Configure whitelist
    aci_gap_configure_whitelist();
}

static void gap_advertise_start(GapState new_state) {
    tBleStatus status;
    uint16_t min_interval;
    uint16_t max_interval;

    FURI_LOG_D(TAG, "Start: %d", new_state);

    if(new_state == GapStateAdvFast) {
        min_interval = 0x80; // 80 ms
        max_interval = 0xa0; // 100 ms
    } else {
        min_interval = 0x0640; // 1 s
        max_interval = 0x0fa0; // 2.5 s
    }
    // Stop advertising timer
    furi_timer_stop(gap->advertise_timer);

    if((new_state == GapStateAdvLowPower) &&
       ((gap->state == GapStateAdvFast) || (gap->state == GapStateAdvLowPower))) {
        // Stop advertising
        status = aci_gap_set_non_discoverable();
        if(status) {
            FURI_LOG_E(TAG, "set_non_discoverable failed %d", status);
        } else {
            FURI_LOG_D(TAG, "set_non_discoverable success");
        }
    }

    if(gap->service.mfg_data_len > 0) {
        hci_le_set_scan_response_data(gap->service.mfg_data_len, gap->service.mfg_data);
    }

    // Configure advertising
    status = aci_gap_set_discoverable(
        ADV_IND,
        min_interval,
        max_interval,
        CFG_IDENTITY_ADDRESS,
        0,
        strlen(gap->service.adv_name),
        (uint8_t*)gap->service.adv_name,
        gap->service.adv_svc_uuid_len,
        gap->service.adv_svc_uuid,
        0,
        0);
    if(status) {
        FURI_LOG_E(TAG, "set_discoverable failed %d", status);
    }
    gap->state = new_state;
    GapEvent event = {.type = GapEventTypeStartAdvertising};
    gap->on_event_cb(event, gap->context);
    furi_timer_start(gap->advertise_timer, INITIAL_ADV_TIMEOUT);
}

static void gap_advertise_stop(void) {
    FURI_LOG_D(TAG, "Stop");
    tBleStatus ret;
    if(gap->state > GapStateIdle) {
        if(gap->state == GapStateConnected) {
            // Terminate connection
            ret = aci_gap_terminate(gap->service.connection_handle, 0x13);
            if(ret != BLE_STATUS_SUCCESS) {
                FURI_LOG_E(TAG, "terminate failed %d", ret);
            } else {
                FURI_LOG_D(TAG, "terminate success");
            }
        }
        // Stop advertising
        furi_timer_stop(gap->advertise_timer);
        ret = aci_gap_set_non_discoverable();
        if(ret != BLE_STATUS_SUCCESS) {
            FURI_LOG_E(TAG, "set_non_discoverable failed %d", ret);
        } else {
            FURI_LOG_D(TAG, "set_non_discoverable success");
        }
        gap->state = GapStateIdle;
    }
    GapEvent event = {.type = GapEventTypeStopAdvertising};
    gap->on_event_cb(event, gap->context);
}

void gap_start_advertising(void) {
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    if(gap->state == GapStateIdle) {
        gap->state = GapStateStartingAdv;
        FURI_LOG_I(TAG, "Start advertising");
        gap->enable_adv = true;
        GapCommand command = GapCommandAdvFast;
        furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

void gap_stop_advertising(void) {
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    if(gap->state > GapStateIdle) {
        FURI_LOG_I(TAG, "Stop advertising");
        gap->enable_adv = false;
        GapCommand command = GapCommandAdvStop;
        furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

static void gap_advetise_timer_callback(void* context) {
    UNUSED(context);
    GapCommand command = GapCommandAdvLowPower;
    furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
}

bool gap_init(GapConfig* config, GapEventCallback on_event_cb, void* context) {
    if(!ble_glue_is_radio_stack_ready()) {
        return false;
    }

    furi_check(gap == NULL);

    gap = malloc(sizeof(Gap));
    gap->config = config;
    // Create advertising timer
    gap->advertise_timer = furi_timer_alloc(gap_advetise_timer_callback, FuriTimerTypeOnce, NULL);
    // Initialization of GATT & GAP layer
    gap->service.adv_name = config->adv_name;
    gap_init_svc(gap);
    ble_event_dispatcher_init();
    // Initialization of the GAP state
    gap->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gap->state = GapStateIdle;
    gap->service.connection_handle = 0xFFFF;
    gap->enable_adv = true;

    // Command queue allocation
    gap->command_queue = furi_message_queue_alloc(8, sizeof(GapCommand));

    // Thread configuration
    gap->thread = furi_thread_alloc_ex("BleGapDriver", 1024, gap_app, gap);
    furi_thread_start(gap->thread);

    // Set initial state
    gap->is_secure = false;
    gap->negotiation_round = 0;

    if(gap->config->mfg_data_len > 0) {
        // Offset by 2 for length + AD_TYPE_MANUFACTURER_SPECIFIC_DATA
        gap->service.mfg_data_len = 2;
        set_manufacturer_data(gap->config->mfg_data, gap->config->mfg_data_len);
    }

    if(gap->config->adv_service.UUID_Type == UUID_TYPE_16) {
        uint8_t adv_service_uid[2];
        gap->service.adv_svc_uuid_len = 1;
        adv_service_uid[0] = gap->config->adv_service.Service_UUID_16 & 0xff;
        adv_service_uid[1] = gap->config->adv_service.Service_UUID_16 >> 8;
        set_advertisment_service_uid(adv_service_uid, sizeof(adv_service_uid));
    } else if(gap->config->adv_service.UUID_Type == UUID_TYPE_128) {
        gap->service.adv_svc_uuid_len = 1;
        set_advertisment_service_uid(
            gap->config->adv_service.Service_UUID_128,
            sizeof(gap->config->adv_service.Service_UUID_128));
    } else {
        furi_crash("Invalid UUID type");
    }

    // Set callback
    gap->on_event_cb = on_event_cb;
    gap->context = context;

    return true;
}

GapState gap_get_state(void) {
    GapState state;
    if(gap) {
        furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        state = gap->state;
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    } else {
        state = GapStateUninitialized;
    }
    return state;
}

void gap_thread_stop(void) {
    if(gap) {
        furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        gap->enable_adv = false;
        GapCommand command = GapCommandKillThread;
        furi_message_queue_put(gap->command_queue, &command, FuriWaitForever);
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
        furi_thread_join(gap->thread);
        furi_thread_free(gap->thread);
        gap->thread = NULL;
        // Free resources
        furi_mutex_free(gap->state_mutex);
        gap->state_mutex = NULL;
        furi_message_queue_free(gap->command_queue);
        gap->command_queue = NULL;
        furi_timer_free(gap->advertise_timer);
        gap->advertise_timer = NULL;

        ble_event_dispatcher_reset();
        free(gap);
        gap = NULL;
    }
}

static int32_t gap_app(void* context) {
    UNUSED(context);
    GapCommand command;
    while(1) {
        FuriStatus status = furi_message_queue_get(gap->command_queue, &command, FuriWaitForever);
        if(status != FuriStatusOk) {
            FURI_LOG_E(TAG, "Message queue get error: %d", status);
            continue;
        }
        furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        if(command == GapCommandKillThread) {
            break;
        }
        if(command == GapCommandAdvFast) {
            gap_advertise_start(GapStateAdvFast);
        } else if(command == GapCommandAdvLowPower) {
            gap_advertise_start(GapStateAdvLowPower);
        } else if(command == GapCommandAdvStop) {
            gap_advertise_stop();
        }
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    }

    return 0;
}

void gap_emit_ble_beacon_status_event(bool active) {
    GapEvent event = {.type = active ? GapEventTypeBeaconStart : GapEventTypeBeaconStop};
    gap->on_event_cb(event, gap->context);
    FURI_LOG_I(TAG, "Beacon status event: %d", active);
}
