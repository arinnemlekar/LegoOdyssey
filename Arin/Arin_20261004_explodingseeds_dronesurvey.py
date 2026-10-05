from pybricks.hubs import PrimeHub
from pybricks.pupdevices import Motor, ColorSensor, UltrasonicSensor
from pybricks.parameters import Button, Color, Direction, Port, Side, Stop, Icon
from pybricks.robotics import DriveBase
from pybricks.tools import wait, Matrix, multitask, run_task

hub = PrimeHub()
hub.system.set_stop_button((Button.BLUETOOTH))

# Robot configuration
motorattachmentleft = Motor(Port.A)
motorattachmentright = Motor(Port.E)

#left_color_sensor = ColorSensor(Port.C)
#right_color_sensor = ColorSensor(Port.D)+

motordriveleft = Motor(Port.F, Direction.COUNTERCLOCKWISE)
motordriveright = Motor(Port.B)
drivebase = DriveBase(motordriveleft, motordriveright, wheel_diameter = 62, axle_track = 108)
drivebase.use_gyro(True)

# Wait for IMU to calibrate
hub.light.on(Color.RED)
while not hub.imu.ready(): pass
hub.light.on(Color.MAGENTA)

# ================================ Helper functions ================================

def stopmotors() -> None:
    """Stops all motors to coast.

    """

    motorattachmentleft.stop()
    motorattachmentright.stop()
    drivebase.stop()

def turnto(angle: int) -> None:
    """Turns to the specified absolute angle.

    Arguments:
        angle (int, deg): The target angle

    """

    drivebase.turn(angle - hub.imu.heading())

def initrun(angle: int) -> None:
    """Starts the run by ramming backward into the wall.

    Arguments:
        angle (int, deg): The angle the robot is initially facing

    """

    hub.system.set_stop_button((Button.CENTER))

    # Drive backwards a bit, unregulated
    motordriveleft.dc(-20) #50
    motordriveright.dc(-20) #50
    wait(500)
    drivebase.brake()
    resetangles(angle)

def resetangles(angle: int) -> None:
    """Resets the angles of the drivebase and attachment motors.
    Arguments:
        angle (int, deg): The angle the robot is initially facing
    """

    # Reset angles
    hub.imu.reset_heading(angle)
    motordriveleft.reset_angle(0)
    motordriveright.reset_angle(0)
    drivebase.reset()
    motorattachmentleft.reset_angle(0)
    motorattachmentright.reset_angle(0)

def catcherr(err: BaseException) -> bool:
    stopmotors()

    print(repr(err))

    if isinstance(err, SystemExit) and not any(hub.buttons.pressed()):
        # If a SystemExit is triggered but no button was pressed on the hub,
        # it means you clicked the abort button in the IDE

        hub.speaker.beep(50, 50)
        return True

    hub.light.on(Color.RED)
    hub.speaker.beep(100, 50)
    return False

def color_alignment() -> None:
    """Aligns the robot to a black line using the left and right color sensors.
    """
   #decrease sensor light intensity to reduce interference between sensors
    left_color_sensor.lights.on((0,10,0))
    right_color_sensor.lights.on((10,0,0))
    side_color_detected = ""
    drivebase.settings(150,(120,120), 100, 100) #Reduce acceleration and deceleration to improve drive consistency
    #Move forward until one color sensor detects black line
    while True:
        reflected_r = right_color_sensor.reflection()
        reflected_l = left_color_sensor.reflection()
        print("reflected_l = ", reflected_l, " ; reflected_r = ", reflected_r)
        drivebase.drive(150,0)
        if reflected_l < 5 and reflected_l > 0:  #if detect black on left sensor
            side_color_detected = "left"
            break
        if reflected_r < 5 and reflected_r > 0:  #if detect black on right sensor
            side_color_detected = "right"
            break
    drivebase.brake()
    print("side_color_detected: ", side_color_detected)
    #Turn the opposite wheel until the opposite sensor detects black also
    while True:
        reflected_r = right_color_sensor.reflection()
        reflected_l = left_color_sensor.reflection()
        print("reflected_l = ", reflected_l, " ; reflected_r = ", reflected_r)
        if side_color_detected == "left": #if first detected black on left side, turn right wheel until right sensor detects black
            motordriveright.run(300)
            if reflected_r < 5 and reflected_r > 0:
                break
        if side_color_detected == "right": #if first detected black on right side, turn right wheel until left sensor detects black
            motordriveleft.run(300)
            if reflected_l < 5 and reflected_l > 0:
                break
    drivebase.stop()

# Wrap attachment motor runs in async functions for multitasking
async def run_left_attachment_motor(speed: int, turn_angle: int):
    await motorattachmentleft.run_angle(speed, turn_angle)
async def run_right_attachment_motor(speed: int, turn_angle: int):
    await motorattachmentright.run_angle(speed, turn_angle)

def vertical_lift(motor_speed: int, motor_turn_angle: int):
    """Lifts the attachment vertically by running both attachment motors.

    Arguments:
        motor_speed (int, deg/s): The speed to run the attachment motors
        motor_turn_angle (int, deg): The angle to turn the attachment motors (positive is up)
    """
    run_task(multitask( run_left_attachment_motor(motor_speed, -motor_turn_angle),
                        run_right_attachment_motor(motor_speed, motor_turn_angle)))

# Wrap drive motor runs in async functions for multitasking
async def run_left_drive_motor(speed: int, time: int):
    await motordriveleft.run_time(speed, time, then=Stop.HOLD)
async def run_right_drive_motor(speed: int, time: int):
    await motordriveright.run_time(speed, time, then=Stop.HOLD)

def wall_running(direction: str, side_of_wall: str, speed: int, time: int):
    """ Move the drivebase parallel to the wall.

    Arguments:
        direction (str, "forward" or "backward"): direction the drivebase is moving at
        side_of_wall (str, "left" or "right"): which side is the wall to drivebase
        speed (int, deg/s): The speed to run the drivebase
        time (int, ms): The time to run the drivebase motors
    """
    offset = speed*0.043 # 4.3% speed offset to the motors to maintain wall running
    if direction == "backward":
        speed = -speed
        offset = -offset
    if side_of_wall =="right":
        run_task(multitask( run_left_drive_motor(speed+offset, time),
                            run_right_drive_motor(speed, time),))
    if side_of_wall =="left":
        run_task(multitask( run_right_drive_motor(speed+offset, time),
                            run_left_drive_motor(speed, time)))

def straightto(heading: int, distance: int) -> None:
    """Turns to absolute heading then drives straight, eliminating compounding heading error."""
    turnto(heading)
    drivebase.straight(distance)

def arc_to_heading(radius: int, target_heading: int) -> None:
    """Arcs to an absolute target heading rather than a relative angle."""
    drivebase.arc(radius, target_heading - hub.imu.heading())

def wall_reset(angle: int, drive_time: int = 400) -> None:
    """Drives backward into a wall to re-anchor position and heading mid-run.

    Arguments:
        angle (int, deg): The heading after re-anchoring
        drive_time (int, ms): How long to drive backward (tune so robot reaches wall)
    """
    motordriveleft.dc(-40)
    motordriveright.dc(-40)
    wait(drive_time)
    drivebase.brake()
    resetangles(angle)

def battery_speed(base_speed: int) -> int:
    """Scales speed up to compensate for battery voltage drop below nominal 8V."""
    return int(base_speed * 8000 / hub.battery.voltage())

def home_motor(motor: Motor, speed: int = 200) -> None:
    """Drives attachment motor to hard stop and resets angle to 0 for consistent starting position.
    Requires a physical hard stop in the negative direction; flip speed sign if stop is in positive direction.
    """
    motor.run_until_stalled(-speed, duty_limit=40)
    motor.reset_angle(0)

def drive_until_color(speed: int, threshold: int = 5) -> None:
    """Drives at given speed until either color sensor detects a black line."""
    drivebase.drive(speed, 0)
    while True:
        if left_color_sensor.reflection() < threshold or \
           right_color_sensor.reflection() < threshold:
            break
    drivebase.brake()

# ================================ Run code ================================
# Run 1: Mission 9 & 10
def run1() -> None:
    global runindex
   # initrun(0)
    
    drivebase.straight(670)
    motorattachmentleft.run_angle(speed = 300, rotation_angle = 300)
    motorattachmentleft.run_angle(speed = 300, rotation_angle = -100)
    drivebase.straight(-690)



    runindex += 1


    #Run 2: Mission 5 & 6 & 7
def run2() -> None:
    global runindex

    #initrun(0)

    drivebase.straight(190)
    turnto(45)
    drivebase.straight(300)
    motorattachmentright.run_angle(speed = 300, rotation_angle = -65)
    
    drivebase.straight(-70)
    wait(1000)
    turnto(-0)
    '''drivebase.straight(750)
    turnto(-52)
    drivebase.straight(200)
    motorattachmentleft.run_angle(speed = 300, rotation_angle = 300)
    drivebase.straight(-200)
    turnto(0)
    drivebase.straight(-750)'''

# Run 3: Mission 8 and 13
def run3() -> None:
    global runindex

    
    # initrun(0)
    drivebase.straight(180)
    motorattachmentright.run_angle(speed = 250, rotation_angle = -250)
    drivebase.turn(45)
   

    runindex += 1

# Run 4: Mission 11
def run4() -> None:
    global runindex

    
    initrun(0)

    drivebase.settings(950, (600,400), 500, 500)
    drivebase.straight(900)
    turnto(46)
    motorattachmentright.run_angle(speed = battery_speed(5000), rotation_angle = 650)
    drivebase.straight(250)
    turnto(70)
    motorattachmentright.run_angle(speed = battery_speed(1000), rotation_angle = -650)
    straightto(-30, 850)


    runindex += 1

# Mission 1 and 2
def run5() -> None:
    global runindex
    resetangles(0)
    drivebase.settings(400, 500, 200, 200)
    drivebase.straight(755)
    straightto(-46, 175)
    motorattachmentright.run_angle(battery_speed(500), 120)
    drivebase.straight(-260)
    straightto(-90, 225)
    motorattachmentleft.run_angle(200, -100)
    arc_to_heading(30, -165)
    drivebase.settings(950)
    drivebase.straight(500)
    drivebase.settings(default_drivebase_settings[0],
                       default_drivebase_settings[1],
                       default_drivebase_settings[2],
                       default_drivebase_settings[3])

    runindex += 1

# Mission 12 and 15 (1) and 11
def run6() -> None:
    global runindex
    wall_running("forward","right",600, 2100)
    motorattachmentleft.run_angle(battery_speed(200), -400)
    motorattachmentright.run_angle(battery_speed(900), 1600)
    motorattachmentleft.run_angle(battery_speed(900), 400)
    drivebase.settings(500,500,500,500)
    drivebase.straight(-500)
    drivebase.settings(default_drivebase_settings[0],
                       default_drivebase_settings[1],
                       default_drivebase_settings[2],
                       default_drivebase_settings[3])

    runindex += 1

#Mission 14: 4 items
def run7() -> None:
    global runindex
    drivebase.settings(350, (500,300), 100, 100)
    drivebase.arc(600, 42, then=Stop.COAST)
    drivebase.settings(900, 500, 100, 100)
    drivebase.straight(-400)
    print("battery voltage: ", hub.battery.voltage(), ", current: ", hub.battery.current())
    drivebase.settings(default_drivebase_settings[0],
                       default_drivebase_settings[1],
                       default_drivebase_settings[2],
                       default_drivebase_settings[3])
    runindex +=1

# Mission 3, 4, 13 and 15 (1) Version 1
def run8() -> None:
    print("battery voltage: ", hub.battery.voltage(), ", current: ", hub.battery.current())
    drivebase.settings(250, (400,200), 100, 100)
    drivebase.straight(850, then=Stop.HOLD)
    drivebase.turn(55.1)
    vertical_lift(battery_speed(400), 768)
    drivebase.settings(100,100,100,100)
    drivebase.arc(468, 28) # Forklift under artifact
    vertical_lift(battery_speed(150), 316.3)
    wait(1500)
    vertical_lift(battery_speed(250), 298.2)
    drivebase.arc(468,None,-100) # Pull artifact out of structure
    drivebase.turn(69)
    vertical_lift(battery_speed(800),-1360)
    drivebase.settings(300, (400,200), 150, 150)
    drivebase.straight(345)

    #drivebase.straight(50)  Forklift under statue's neck
    #drivebase.turn(7)
    drivebase.settings(300, 500, 500, 500)
    vertical_lift(battery_speed(800),820)
    drivebase.turn(-25) # Push statue to upright position
    #drivebase.straight(-30)
    drivebase.settings(500,500,500,500)
    drivebase.turn(12)
    vertical_lift(battery_speed(900), -820)
    wait(50)
    drivebase.straight(-165)
    print("battery voltage: ", hub.battery.voltage(), ", current: ", hub.battery.current())
    drivebase.settings(default_drivebase_settings[0],
                       default_drivebase_settings[1],
                       default_drivebase_settings[2],
                       default_drivebase_settings[3])
    runindex +=1


# Mission 3, 4 and 15 (1), Version 2
def run9() -> None:
    print("battery voltage: ", hub.battery.voltage(), ", current: ", hub.battery.current())
    drivebase.settings(250, (400,200), 100, 100)
    drivebase.straight(850, then=Stop.HOLD)
    drivebase.turn(55.1)
    vertical_lift(battery_speed(800), 768) # Initial Raising
    drivebase.settings(100,100,100,100)
    drivebase.arc(468, 28) # Forklift under artifact
    vertical_lift(battery_speed(150), 316.3) # Raising the artifact and the red bar
    wait(1500) # Waiting for Minecart to cross
    vertical_lift(battery_speed(250), 298.2) # Further pull up to avoid obstacles
    drivebase.arc(468,None,-100) # Pull artifact out of structure
    drivebase.turn(75)
    vertical_lift(battery_speed(950),-1360)
    drivebase.settings(300, (400,200), 150, 150)
    drivebase.straight(305)

    #drivebase.straight(50)  Forklift under statue's neck
    #drivebase.turn(7)
    drivebase.settings(300, 500, 500, 500)
    """vertical_lift(800,820)
    drivebase.turn(-25) # Push statue to upright position
    #drivebase.straight(-30)
    drivebase.settings(500,500,500,500)
    drivebase.turn(12)
    vertical_lift(900, -820)
    wait(50)"""
    drivebase.arc(500, None, -115)

    print("battery voltage: ", hub.battery.voltage(), ", current: ", hub.battery.current())
    drivebase.settings(default_drivebase_settings[0],
                       default_drivebase_settings[1],
                       default_drivebase_settings[2],
                       default_drivebase_settings[3])
    runindex +=1

# ================================ Image bitmaps ================================

digits = [
    Matrix([[0,1,1,1,0],
            [0,1,0,1,0],
            [0,1,0,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,0,1,0,0],
            [0,1,1,0,0],
            [0,0,1,0,0],
            [0,0,1,0,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,0,0,1,0],
            [0,1,1,1,0],
            [0,1,0,0,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,0,0,1,0],
            [0,1,1,1,0],
            [0,0,0,1,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,0,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0],
            [0,0,0,1,0],
            [0,0,0,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,1,0,0,0],
            [0,1,1,1,0],
            [0,0,0,1,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,1,0,0,0],
            [0,1,1,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,0,0,1,0],
            [0,0,1,0,0],
            [0,1,0,0,0],
            [0,1,0,0,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0]]) * 100,

    Matrix([[0,1,1,1,0],
            [0,1,0,1,0],
            [0,1,1,1,0],
            [0,0,0,1,0],
            [0,1,1,1,0]]) * 100,
]

# ================================ Master program ================================

# You can add more runs by adding another entry in the list below
runindex: int = 0
runs = [ # [Icon, Callback]
    [digits[1], run1],
    [digits[2], run2],
    [digits[3], run3],
    [digits[4], run4],
    [digits[5], run5],
    [digits[6], run6],
    [digits[7], run7],
    [digits[8], run8],
    [digits[9], run9]
]
numruns: int = len(runs)

#capture default drivebase settings
default_drivebase_settings = drivebase.settings()

# Program is ready
hub.speaker.beep(900, 100)

while True:
    stopmotors()

    # Show selection
    hub.light.on(Color.GREEN)
    hub.display.icon(runs[runindex][0])

    # Wait for button press
    buttons = []
    while not any(buttons): buttons = hub.buttons.pressed()

    if Button.CENTER in buttons:
        # "Run selected" pressed
        hub.speaker.beep(1100, 50)

        # Wait for button released
        while any(hub.buttons.pressed()): pass
        hub.light.on(Color.WHITE)
        hub.display.off()

        try:
             # Load default drivebase settings
            drivebase.settings( default_drivebase_settings[0],
                                default_drivebase_settings[1],
                                default_drivebase_settings[2],
                                default_drivebase_settings[3])
            # Do selected run
            runs[runindex][1]()
            stopmotors()
            hub.speaker.beep(800, 50)

        except BaseException as err:
            # An error happened
            if catcherr(err): raise # It was the abort button in the IDE, so end the program

        runindex %= numruns
        hub.system.set_stop_button((Button.BLUETOOTH))
        while Button.CENTER in hub.buttons.pressed(): pass

    elif Button.RIGHT in buttons:
        # "Next" pressed
        runindex = (runindex + 1) % numruns
        wait(200)
        hub.speaker.beep(600, 50)

    elif Button.LEFT in buttons:
        # "Previous" pressed
        runindex = (runindex + numruns - 1) % numruns
        wait(200)
        hub.speaker.beep(550, 50)
