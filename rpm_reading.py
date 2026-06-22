#!/usr/bin/env python3
"""
Raspberry Pi 5 Fan RPM Monitor Service
Tachometer Pin: GPIO16 (Physical Pin 36)

Features:
- Real-time RPM monitoring via tachometer signal
- Reports current RPM every 2 seconds
- Simple output: only current RPM value
- Most fans generate 2 pulses per revolution
"""

import time
import logging
import signal
import sys
import os
import threading
from gpiozero import Button

# Setup logging - simplified format
logging.basicConfig(
    level=logging.INFO,
    format='%(message)s',  # Only show the message, no timestamp or level
    handlers=[
        logging.FileHandler('/var/log/rpm-monitor.log'),
        logging.StreamHandler(sys.stdout)
    ]
)

class RPMMonitor:
    def __init__(self, tach_pin=16):
        """
        Initialize RPM Monitor
        """
        self.shutdown = False
        
        # Setup signal handlers
        signal.signal(signal.SIGTERM, self.signal_handler)
        signal.signal(signal.SIGINT, self.signal_handler)
        
        try:
            # Initialize tachometer input
            self.tach = Button(tach_pin, pull_up=True, bounce_time=0.001)
            
            # RPM calculation variables
            self.rpm_count = 0
            self.current_rpm = 0
            
            # Thread control
            self.monitor_thread_running = True
            
            # Setup tachometer interrupt
            self.tach.when_pressed = self._tach_callback
            
            # Start RPM calculation thread
            self.monitor_thread = threading.Thread(target=self._rpm_calculation, daemon=True)
            self.monitor_thread.start()
            
            print("RPM Monitor started - Reporting every 2 seconds")
            print("Current RPM:")
            
        except Exception as e:
            print(f"Failed to initialize RPM monitor: {e}")
            self.cleanup()
            sys.exit(1)
    
    def signal_handler(self, signum, frame):
        """Handle shutdown signals"""
        print("\nShutting down RPM monitor...")
        self.shutdown = True
        self.cleanup()
        sys.exit(0)
    
    def _tach_callback(self):
        """Tachometer pin callback - counts each rising edge"""
        self.rpm_count += 1
    
    def _rpm_calculation(self):
        """RPM calculation thread"""
        while self.monitor_thread_running and not self.shutdown:
            # Store current count and reset
            pulse_count = self.rpm_count
            self.rpm_count = 0
            
            # Calculate RPM
            # Most 4-wire PWM fans generate 2 pulses per revolution
            # RPM = (pulses per second) × 60 seconds / 2 pulses per revolution
            if pulse_count > 0:
                self.current_rpm = (pulse_count / 2) * 60
            else:
                self.current_rpm = 0
            
            # Wait for next measurement
            time.sleep(1)
    
    def get_current_rpm(self):
        """Get current RPM reading"""
        return self.current_rpm
    
    def run(self):
        """Main monitoring loop - reports current RPM every 2 seconds"""
        error_count = 0
        max_errors = 10
        
        while not self.shutdown and error_count < max_errors:
            try:
                # Get current RPM
                current_rpm = self.get_current_rpm()
                
                # Display only the current RPM value
                print(f"{current_rpm:.0f}")
                
                error_count = 0  # Reset error count on success
                
                # Wait 2 seconds before next report
                time.sleep(2)
                    
            except Exception as e:
                error_count += 1
                print(f"Error: {e}")
                time.sleep(5)
        
        if error_count >= max_errors:
            print("Too many errors, shutting down")
    
    def cleanup(self):
        """Clean up resources"""
        self.monitor_thread_running = False
        try:
            if hasattr(self, 'tach') and self.tach:
                self.tach.close()
        except Exception as e:
            print(f"Error cleaning up: {e}")

def main():
    """Main function"""
    try:
        monitor = RPMMonitor(tach_pin=16)
        monitor.run()
            
    except KeyboardInterrupt:
        print("\nRPM Monitor stopped")
    except Exception as e:
        print(f"Fatal error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()