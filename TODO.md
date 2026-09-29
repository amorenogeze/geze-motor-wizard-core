# Actual status

### CORE
#### WIZARD-ENGINE
- [x] wizard-ui.socket. Socket between ui<-->engine
- [x] wizard-backend.socket. Socket between engine<-->gateway
- [] CONFIGURATOR - read compare and match config between UI and real MCU.
- [] ErrorLog -
- [] EventLog - Do we want a file with timestamp with all the events? What is an event? powering UP? or just commands and WIZARD stuff?
- [] 
 
#### DEVICE-GATEWAY
- [x] read from vCAN0. Receiving daat from CANsimulator.py. SOLO PICO in the near future.
- [x] send data to wizard-backend.socket
- [] implement real PICO scenario
- [] 
- []
