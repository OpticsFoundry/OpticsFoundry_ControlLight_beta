import sys

#For quick testing of latest compilation of library
#main_path = "D:\\Florian\\Firefly\\FireflyControl\\ControlLight\\"
#library_path = main_path + "out\\build\\x64-debug"

#For more permanent installation, copy 
# control_light_api.cp310-win_amd64.pyd
# ControlLight.dll
# from the library creation path, e.g. D:\Florian\Firefly\FireflyControl\ControlLight\out\build\x64-debug,
# to the "lib" subdirectory of this script
main_path = ""
library_path = "lib"

sys.path.append(library_path)
import control_light_api

def show_sequencer_network_settings(cla):
    # Network settings of the sequencers (needs sequencer firmware from 2026-11-27 on).
    # By default (DHCP) a sequencer gets its IP address from a DHCP server; without DHCP server (e.g. connected directly to the PC)
    # it uses after 3 s a link-local address 169.254.x.y, as a PC does. A fixed IP address is useful if the PC's network adapter has a fixed IP address.
    # Discovery works without initialized system and finds sequencers also if their IP address is in another subnet than the PC.
    for sequencer in cla.discover_sequencers(timeout_in_seconds = 1.0):
        print("Found:", sequencer)
    print("Network settings of sequencer 0:", cla.get_sequencer_network_settings(0))
    # Changing the setting stores it in the sequencer and reboots it (about 10 s); afterwards enter its new IP address in ControlHardwareConfig.json:
    #cla.set_sequencer_fixed_ip(0, "192.168.1.10", "255.255.255.0", gateway = "")
    #cla.set_sequencer_dhcp(0)
    # If a sequencer can't be reached with its IP address, set it by its MAC address (shown by the discovery above) with a UDP broadcast,
    # which works without initialized system and across subnets. Or hold the Z-turn USR button while switching it on, until it beeps twice: back to DHCP.
    #cla.set_sequencer_fixed_ip_by_mac("02:4F:46:4C:B5:52", "192.168.1.10", "255.255.255.0")
    #cla.set_sequencer_dhcp_by_mac("02:4F:46:4C:B5:52")

cla = control_light_api.ControlLightAPI()
print("Created:", cla.is_created())
try:
    cla.load_from_json_file(main_path+r"ControlHardwareConfig.json")
except Exception as e:
    print("Error loading JSON file:", e)
    sys.exit(1)
try:
    cla.initialize()
    show_sequencer_network_settings(cla)
    cla.switch_debug_mode(True, "DebugSequence")
    start_time = 0
    for i in range(10):
        cla.start_assembling_sequence()
        cla.sequencer_start_analog_in_acquisition(
            0,
            0,  # AQuRA MCP3208 analog in board
            0,  # SPI chip-select
            0,
            100,
            0.1)
        for j in range(100):
            cla.set_voltage(0, 24, 10.0 * j / 100.0)
            cla.wait_ms(0.1)
            frequency = 1000.0 * j / 100.0
            cla.set_frequency(0, 232, frequency)

        cla.execute_sequence()

        input = cla.wait_till_end_of_sequence_then_get_input_data(timeout_in_s = 10)
        print("Input data length:", input["length"]," duration:", input["end_time_of_cycle"]-start_time)
        start_time = input["end_time_of_cycle"]
except Exception as e:
    print("Error:", e)
    sys.exit(1)
