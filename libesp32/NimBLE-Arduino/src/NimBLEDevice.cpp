/*
 * NimBLEDevice.cpp
 *
 *  Created: on Jan 24 2020
 *      Author H2zero
 * 
 * Originally:
 *
 * BLEDevice.cpp
 *
 *  Created on: Mar 16, 2017
 *      Author: kolban
 */
#include "sdkconfig.h"
#if defined(CONFIG_BT_ENABLED)

#include "NimBLEDevice.h"
#include "NimBLEUtils.h"

#include "esp_err.h"
#include "esp_bt.h"
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#ifdef ARDUINO_ARCH_ESP32
#include "esp32-hal-bt.h"
#endif

#include "NimBLELog.h"

static const char* LOG_TAG = "NimBLEDevice";

/**
 * Singletons for the NimBLEDevice.
 */
bool            initialized = false;
NimBLEScan*     NimBLEDevice::m_pScan = nullptr;
NimBLEServer*   NimBLEDevice::m_pServer = nullptr;
uint32_t        NimBLEDevice::m_passkey = 123456;
bool            NimBLEDevice::m_synced = false;
NimBLEAdvertising* NimBLEDevice::m_bleAdvertising = nullptr;

gap_event_handler           NimBLEDevice::m_customGapHandler = nullptr;
ble_gap_event_listener      NimBLEDevice::m_listener;
std::list <NimBLEClient*>   NimBLEDevice::m_cList;
std::list <NimBLEAddress>   NimBLEDevice::m_ignoreList;
NimBLESecurityCallbacks*    NimBLEDevice::m_securityCallbacks = nullptr;
  
//std::map<uint16_t, conn_status_t> BLEDevice::m_connectedClientsMap;

//gattc_event_handler BLEDevice::m_customGattcHandler = nullptr;
//gatts_event_handler BLEDevice::m_customGattsHandler = nullptr;


/**
 * @brief Create a new instance of a server.
 * @return A new instance of the server.
 */
/* STATIC */ NimBLEServer* NimBLEDevice::createServer() {
/*#ifndef CONFIG_GATTS_ENABLE  // Check that BLE GATTS is enabled in make menuconfig
    NIMBLE_LOGE(LOG_TAG, "BLE GATTS is not enabled - CONFIG_GATTS_ENABLE not defined");
    abort();
#endif // CONFIG_GATTS_ENABLE
*/
    if(NimBLEDevice::m_pServer == nullptr) {
        NimBLEDevice::m_pServer = new NimBLEServer();
		ble_gatts_reset();
        ble_svc_gap_init();
        ble_svc_gatt_init();
    }

    return m_pServer;
} // createServer


NimBLEAdvertising* NimBLEDevice::getAdvertising() {
	if(m_bleAdvertising == nullptr) {
		m_bleAdvertising = new NimBLEAdvertising();
	}
	return m_bleAdvertising; 
}


void NimBLEDevice::startAdvertising() {
	getAdvertising()->start();
} // startAdvertising


void NimBLEDevice::stopAdvertising() {
    getAdvertising()->stop();
} // stopAdvertising


/**
 * @brief Retrieve the Scan object that we use for scanning.
 * @return The scanning object reference.  This is a singleton object.  The caller should not
 * try and release/delete it.
 */
/* STATIC */ NimBLEScan* NimBLEDevice::getScan() {
    if (m_pScan == nullptr) {
        m_pScan = new NimBLEScan();
    }
    return m_pScan;
} // getScan


/**
 * @brief Creates a new client object and maintains a list of all client objects
 * each client can connect to 1 peripheral device. 
 * @return A reference to the new client object.
 */
/* STATIC */ NimBLEClient* NimBLEDevice::createClient() {
    if(m_cList.size() >= NIMBLE_MAX_CONNECTIONS) {
        NIMBLE_LOGW("Number of clients exceeds Max connections. Max=(%d)", 
                                            NIMBLE_MAX_CONNECTIONS);
    }
    
    NimBLEClient* pClient = new NimBLEClient();
    m_cList.push_back(pClient);

    return pClient;
} // createClient


/**
 * @brief Delete the client object and remove it from the list.
 * Check if it is connected or trying to connect and close/stop it first.
 * @param [in] Pointer to the client object.
 */
/* STATIC */ bool NimBLEDevice::deleteClient(NimBLEClient* pClient) {
    if(pClient == nullptr) {
        return false;
    }
    
    if(pClient->m_isConnected) {
        if (pClient->disconnect() != 0) {
            return false;
        }
        while(pClient->m_isConnected) {
            vTaskDelay(1);
        }
    }
    
    if(pClient->m_waitingToConnect) {
        if(ble_gap_conn_cancel() != 0){
            return false;
        }
        while(pClient->m_waitingToConnect) {
            vTaskDelay(1);
        }
    }
    
    m_cList.remove(pClient);
    delete pClient;
    
    return true;
} // deleteClient


/**
 * @brief get the list of clients.
 * @return a pointer to the list of clients.
 */
/* STATIC */std::list<NimBLEClient*>* NimBLEDevice::getClientList() {
    return &m_cList;
} // getClientList


/**
 * @brief get the size of the list of clients.
 * @return a pointer to the list of clients.
 */
/* STATIC */size_t NimBLEDevice::getClientListSize() {
    return m_cList.size();
} // getClientList


/**
 * @brief Get a reference to a client by connection ID.
 * @param [in] The client connection ID to search for.
 * @return A reference pointer to the client with the spcified connection ID.
 */
/* STATIC */NimBLEClient* NimBLEDevice::getClientByID(uint16_t conn_id) {
    for(auto it = m_cList.cbegin(); it != m_cList.cend(); ++it) {
        if((*it)->getConnId() == conn_id) {
            return (*it);
        }
    }
    assert(0);
    return nullptr;
} // getClientByID


/**
 * @brief Get a reference to a client by peer address.
 * @param [in] a NimBLEAddress of the peer to search for.
 * @return A reference pointer to the client with the peer address.
 */
/* STATIC */NimBLEClient* NimBLEDevice::getClientByPeerAddress(NimBLEAddress peer_addr) {
    for(auto it = m_cList.cbegin(); it != m_cList.cend(); ++it) {
        if((*it)->getPeerAddress().equals(peer_addr)) {
            return (*it);
        }
    }
    return nullptr;
} // getClientPeerAddress


/**
 * @brief Finds the first disconnected client in the list.
 * @return A reference pointer to the first client that is not connected to a peer.
 */
/* STATIC */NimBLEClient* NimBLEDevice::getDisconnectedClient() {
    for(auto it = m_cList.cbegin(); it != m_cList.cend(); ++it) {
        if(!(*it)->isConnected()) {
            return (*it);
        }
    }
    return nullptr;
} // getDisconnectedClient


/**
 * @brief Set the transmission power.
 * The power level can be one of:
 * *   ESP_PWR_LVL_N12 = 0,                !< Corresponding to -12dbm 
 * *   ESP_PWR_LVL_N9  = 1,                !< Corresponding to  -9dbm 
 * *   ESP_PWR_LVL_N6  = 2,                !< Corresponding to  -6dbm 
 * *   ESP_PWR_LVL_N3  = 3,                !< Corresponding to  -3dbm 
 * *   ESP_PWR_LVL_N0  = 4,                !< Corresponding to   0dbm 
 * *   ESP_PWR_LVL_P3  = 5,                !< Corresponding to  +3dbm 
 * *   ESP_PWR_LVL_P6  = 6,                !< Corresponding to  +6dbm 
 * *   ESP_PWR_LVL_P9  = 7,                !< Corresponding to  +9dbm 
 * @param [in] powerLevel.
 */
/* STATIC */ void NimBLEDevice::setPower(esp_power_level_t powerLevel, esp_ble_power_type_t powerType) {
    NIMBLE_LOGD(LOG_TAG, ">> setPower: %d (type: %d)", powerLevel, powerType);
    esp_err_t errRc = esp_ble_tx_power_set(powerType, powerLevel);
    if (errRc != ESP_OK) {
        NIMBLE_LOGE(LOG_TAG, "esp_ble_tx_power_set: rc=%d", errRc);
    }
    NIMBLE_LOGD(LOG_TAG, "<< setPower");
} // setPower


/* STATIC */ int NimBLEDevice::getPower(esp_ble_power_type_t powerType) {

	switch(esp_ble_tx_power_get(powerType)) {
		case ESP_PWR_LVL_N12:
			return -12;
		case ESP_PWR_LVL_N9:
			return -9;
		case ESP_PWR_LVL_N6:
			return -6;
		case ESP_PWR_LVL_N3:
			return -6;
		case ESP_PWR_LVL_N0:
			return 0;
		case ESP_PWR_LVL_P3:
			return 3;
		case ESP_PWR_LVL_P6:
			return 6;
		case ESP_PWR_LVL_P9:
			return 9;
		default:
			return BLE_HS_ADV_TX_PWR_LVL_AUTO;
	}
} // setPower


/**
 * @brief Get our device address.
 * @return A NimBLEAddress object of our public address if we have one,
 * if not then our current random address.
 */
/* STATIC*/ NimBLEAddress NimBLEDevice::getAddress() {
    ble_addr_t addr = {BLE_ADDR_PUBLIC, 0};
    //ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr.val, NULL)
    
    if(BLE_HS_ENOADDR == ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr.val, NULL)) {
        NIMBLE_LOGD(LOG_TAG, "Public address not found, checking random");
        addr.type = BLE_ADDR_RANDOM;
        ble_hs_id_copy_addr(BLE_ADDR_RANDOM, addr.val, NULL);
    }
    
    return NimBLEAddress(addr);
} // getAddress


/**
 * @brief Return a string representation of the address of this device.
 * @return A string representation of this device address.
 */
/* STATIC */ std::string NimBLEDevice::toString() {
    return getAddress().toString();
} // toString


/**
 * @brief Setup local mtu that will be used to negotiate mtu during request from client peer
 * @param [in] mtu Value to set local mtu, should be larger than 23 and lower or equal to
 * BLE_ATT_MTU_MAX = 527
 */
/* STATIC */int NimBLEDevice::setMTU(uint16_t mtu) {
    NIMBLE_LOGD(LOG_TAG, ">> setLocalMTU: %d", mtu);
    
    int rc =  ble_att_set_preferred_mtu(mtu);

    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Could not set local mtu value to: %d", mtu);
    }
    
    NIMBLE_LOGD(LOG_TAG, "<< setLocalMTU");
    return rc;
} // setMTU


/**
 * @brief Get local MTU value set.
 */
/* STATIC */uint16_t NimBLEDevice::getMTU() {
    return ble_att_preferred_mtu();
}


/**
 * @brief Host reset, we pass the message so we don't make calls until resynced.
 */
/* STATIC */  void NimBLEDevice::onReset(int reason)
{
    if(!m_synced) {
        return;
    }
    
    m_synced = false;
    
    if(m_pScan != nullptr) {
        m_pScan->onHostReset();
    }
/*  Not needed    
    if(m_pServer != nullptr) {
        m_pServer->onHostReset();
    }
    
    for(auto it = m_cList.cbegin(); it != m_cList.cend(); ++it) {
        (*it)->onHostReset();
    }
*/    
    if(m_bleAdvertising != nullptr) {
        m_bleAdvertising->onHostReset();
    }
    
    NIMBLE_LOGC(LOG_TAG, "Resetting state; reason=%d, %s", reason, 
                        NimBLEUtils::returnCodeToString(reason));
} // onReset


/**
 * @brief Host resynced with controller, all clear to make calls.
 */
/* STATIC */ void NimBLEDevice::onSync(void)
{
    NIMBLE_LOGI(LOG_TAG, "NimBle host synced.");
    // This check is needed due to potentially being called multiple times in succession
    // If this happens, the call to scan start may get stuck or cause an advertising fault.
    if(m_synced) {
        return;
    }
    
    /* Make sure we have proper identity address set (public preferred) */
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    
    m_synced = true;
    
    if(m_pScan != nullptr) {
        // Restart scanning with the last values sent, allow to clear results.
        m_pScan->start(m_pScan->m_duration, m_pScan->m_scanCompleteCB);
    }
    
    if(m_bleAdvertising != nullptr) {
        // Restart advertisng, parameters should already be set.
        m_bleAdvertising->start();
    }
} // onSync


/**
 * @brief The main host task.
 */
/* STATIC */ void NimBLEDevice::host_task(void *param)
{
    NIMBLE_LOGI(LOG_TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
} // host_task


/**
 * @brief Initialize the %BLE environment.
 * @param deviceName The device name of the device.
 */
/* STATIC */ void NimBLEDevice::init(std::string deviceName) {
    if(!initialized){
        initialized = true; // Set the initialization flag to ensure we are only initialized once.
        
        int rc=0;
        esp_err_t errRc = ESP_OK;
        
#ifdef ARDUINO_ARCH_ESP32
        // make sure the linker includes esp32-hal-bt.c so ardruino init doesn't release BLE memory.
        btStarted();
#endif

        errRc = nvs_flash_init();
    
        if (errRc == ESP_ERR_NVS_NO_FREE_PAGES || errRc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            errRc = nvs_flash_init();
        }

        ESP_ERROR_CHECK(errRc);
        
        ESP_ERROR_CHECK(esp_nimble_hci_and_controller_init());
        
        nimble_port_init();
        
        // Setup callbacks for host events 
        ble_hs_cfg.reset_cb = NimBLEDevice::onReset;
        ble_hs_cfg.sync_cb = NimBLEDevice::onSync;
        
        // Set initial security capabilities
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT; 
        ble_hs_cfg.sm_bonding = 0;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = 1;
        ble_hs_cfg.sm_their_key_dist = 3;
        
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr; /*TODO: Implement handler for this*/
        
        // Set the device name.
        rc = ble_svc_gap_device_name_set(deviceName.c_str());
        assert(rc == 0);

        ble_store_config_init();
        
        nimble_port_freertos_init(NimBLEDevice::host_task);
    }
    // Wait for host and controller to sync before returning and accepting new tasks
    while(!m_synced){
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    //vTaskDelay(200 / portTICK_PERIOD_MS); // Delay for 200 msecs as a workaround to an apparent Arduino environment issue.
} // init


/**
 * @brief Shutdown the NimBLE stack/controller.
 */
/* STATIC */ void NimBLEDevice::deinit() {
    int ret = nimble_port_stop();
    if (ret == 0) {
        nimble_port_deinit();
    
        ret = esp_nimble_hci_and_controller_deinit();
        if (ret != ESP_OK) {
            NIMBLE_LOGE(LOG_TAG, "esp_nimble_hci_and_controller_deinit() failed with error: %d", ret);
        }
        
        initialized = false;
    }
} // deinit


/**
 * @brief Check if the initialization is complete.
 */
bool NimBLEDevice::getInitialized() {
    return initialized;
} // getInitialized


/**
 * @brief Set the authorization mode for this device.
 * @param bonding, if true we allow bonding, false no bonding will be performed.
 * @param mitm, if true we are capable of man in the middle protection, false if not.
 * @param sc, if true we will perform secure connection pairing, false we will use legacy pairing.
 */
/*STATIC*/ void NimBLEDevice::setSecurityAuth(bool bonding, bool mitm, bool sc) {
    NIMBLE_LOGD(LOG_TAG, "Setting bonding: %d, mitm: %d, sc: %d",bonding,mitm,sc);
    ble_hs_cfg.sm_bonding = bonding;
    ble_hs_cfg.sm_mitm = mitm;
    ble_hs_cfg.sm_sc = sc;
} // setSecurityAuth


/**
 * @brief Set the authorization mode for this device.
 * @param A bitmap indicating what modes are supported.
 * The bits are defined as follows:
 ** 0x01 BLE_SM_PAIR_AUTHREQ_BOND                
 ** 0x04 BLE_SM_PAIR_AUTHREQ_MITM                
 ** 0x08 BLE_SM_PAIR_AUTHREQ_SC                  
 ** 0x10 BLE_SM_PAIR_AUTHREQ_KEYPRESS  - not yet supported.         
 ** 0xe2 BLE_SM_PAIR_AUTHREQ_RESERVED  - for reference only.
 */ 
/*STATIC*/void NimBLEDevice::setSecurityAuth(uint8_t auth_req) {
    NimBLEDevice::setSecurityAuth((auth_req & BLE_SM_PAIR_AUTHREQ_BOND)>0,
                                (auth_req & BLE_SM_PAIR_AUTHREQ_MITM)>0,
                                (auth_req & BLE_SM_PAIR_AUTHREQ_SC)>0);
} // setSecurityAuth


/**
 * @brief Set the Input/Output capabilities of this device.
 * @param One of the following:
 ** 0x00 BLE_HS_IO_DISPLAY_ONLY         DisplayOnly IO capability
 ** 0x01 BLE_HS_IO_DISPLAY_YESNO        DisplayYesNo IO capability
 ** 0x02 BLE_HS_IO_KEYBOARD_ONLY        KeyboardOnly IO capability 
 ** 0x03 BLE_HS_IO_NO_INPUT_OUTPUT      NoInputNoOutput IO capability 
 ** 0x04 BLE_HS_IO_KEYBOARD_DISPLAY     KeyboardDisplay Only IO capability
 */
/*STATIC*/ void NimBLEDevice::setSecurityIOCap(uint8_t iocap) {
    ble_hs_cfg.sm_io_cap = iocap;
} // setSecurityIOCap


/**
 * @brief If we are the initiator of the security procedure this sets the keys we will distribute.
 * @param A bitmap indicating which keys to distribute during pairing.
 * The bits are defined as follows:           
 ** 0x01: BLE_SM_PAIR_KEY_DIST_ENC  - Distribute the encryption key.
 ** 0x02: BLE_SM_PAIR_KEY_DIST_ID   - Distribute the ID key (IRK).
 ** 0x04: BLE_SM_PAIR_KEY_DIST_SIGN
 ** 0x08: BLE_SM_PAIR_KEY_DIST_LINK
 */
/*STATIC*/void NimBLEDevice::setSecurityInitKey(uint8_t init_key) {
    ble_hs_cfg.sm_our_key_dist = init_key;
} // setsSecurityInitKey


/**
 * @brief Set the keys we are willing to accept during pairing.
 * @param A bitmap indicating which keys to accept during pairing.
 * The bits are defined as follows:           
 ** 0x01: BLE_SM_PAIR_KEY_DIST_ENC  -  Accept the encryption key.
 ** 0x02: BLE_SM_PAIR_KEY_DIST_ID   -  Accept the ID key (IRK).
 ** 0x04: BLE_SM_PAIR_KEY_DIST_SIGN
 ** 0x08: BLE_SM_PAIR_KEY_DIST_LINK
 */
/*STATIC*/void NimBLEDevice::setSecurityRespKey(uint8_t init_key) {
    ble_hs_cfg.sm_their_key_dist = init_key;
} // setsSecurityRespKey


/**
 * @brief Set the passkey for pairing.
 */
/*STATIC*/void NimBLEDevice::setSecurityPasskey(uint32_t pin) {
    m_passkey = pin;
} // setSecurityPasskey


/**
 * @brief Get the passkey for pairing.
 */
/*STATIC*/uint32_t NimBLEDevice::getSecurityPasskey() {
    return m_passkey;
} // getSecurityPasskey


/**
 * @brief Set callbacks that will be used to handle encryption negotiation events and authentication events
 * @param [in] cllbacks Pointer to NimBLESecurityCallbacks class
 */
void NimBLEDevice::setSecurityCallbacks(NimBLESecurityCallbacks* callbacks) {
    NimBLEDevice::m_securityCallbacks = callbacks;
} // setSecurityCallbacks


/**
 * @brief Start the connection securing and authorization for this connection.
 * @param Connection id of the client.
 * @returns host return code 0 if success.
 */
/* STATIC */int NimBLEDevice::startSecurity(uint16_t conn_id) {
  /*  if(m_securityCallbacks != nullptr) {
        m_securityCallbacks->onSecurityRequest(); 
    }
  */  
    int rc = ble_gap_security_initiate(conn_id);
    if(rc != 0){
        NIMBLE_LOGE(LOG_TAG, "ble_gap_security_initiate: rc=%d %s", rc, NimBLEUtils::returnCodeToString(rc));
    }
    
    return rc;
} // startSecurity


/**
 * @brief Check if the device address is on our ignore list.
 * @return True if ignoring.
 */
/*STATIC*/ bool NimBLEDevice::isIgnored(NimBLEAddress address) {
    for(auto &it : m_ignoreList) {
        if(it.equals(address)){
            return true;
        }
    }

    return false;
}


/**
 * @brief Add a device to the ignore list.
 * @param Address of the device we want to ignore.
 */
/*STATIC*/ void NimBLEDevice::addIgnored(NimBLEAddress address) {
    m_ignoreList.push_back(address);
}


/**
 * @brief Remove a device from the ignore list.
 * @param Address of the device we want to remove from the list.
 */
/*STATIC*/void  NimBLEDevice::removeIgnored(NimBLEAddress address) {
    for(auto it = m_ignoreList.begin(); it != m_ignoreList.end(); ++it) {
        if((*it).equals(address)){
            m_ignoreList.erase(it);
            return;
        }
    }
}


/**
 * @brief Set a custom callback for gap events.
 */
void NimBLEDevice::setCustomGapHandler(gap_event_handler handler) {
    m_customGapHandler = handler;
    int rc = ble_gap_event_listener_register(&m_listener, m_customGapHandler, NULL);
    if(rc == BLE_HS_EALREADY){
        NIMBLE_LOGI(LOG_TAG, "Already listening to GAP events.");
    }
    else{
        assert(rc == 0);
    }
} // setCustomGapHandler


/**
 * @brief Backward compatibility for bluedroid gatt events.
 * NimBLe does not send GATT events
 */
 /*
void BLEDevice::setCustomGattcHandler(gattc_event_handler handler) {
    setCustomGapHandler(handler);
}

void BLEDevice::setCustomGattsHandler(gatts_event_handler handler) {
    setCustomGapHandler(handler);
}
*/
/**********************************************************/


#endif // CONFIG_BT_ENABLED
