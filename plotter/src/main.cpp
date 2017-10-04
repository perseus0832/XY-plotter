#if defined (__USE_LPCOPEN)
#if defined(NO_BOARD_LIB)
#include "chip.h"
#else
#include "board.h"
#endif
#endif

#define USING_USB_CDC   0

#include <cr_section_macros.h>

#include "string.h"

#include "FreeRTOS.h"
#include "user_vcom.h"
#include "task.h"
#include "queue.h"

#include "SerialLog.h"
#include "GParser.h"
#include "Laser.h"
#include "Servo.h"
#include "StepperMotor.h"
#include "Command.h"

void setupHardware();

/* Task Declaration */
void vReceiveTask(void *vParameters);
void vExecuteTask(void *vParameters);
void vCalibrateTask(void *vParameters);


StepperMotor *xmotor;
StepperMotor *ymotor;
Servo *pen;

SerialLog debugSerial;
QueueHandle_t qCommand;

int main(void) {

    setupHardware();

    pen = new Servo();

    qCommand = xQueueCreate(10, sizeof(Command));

    xTaskCreate(vReceiveTask, "Receive Task", configMINIMAL_STACK_SIZE * 3, NULL,
            (tskIDLE_PRIORITY + 1UL), (TaskHandle_t *) NULL);

    xTaskCreate(vExecuteTask, "Execute Task", configMINIMAL_STACK_SIZE * 3, NULL,
            (tskIDLE_PRIORITY + 1UL), (TaskHandle_t *) NULL);

    xTaskCreate(vCalibrateTask, "Calibrate Task", configMINIMAL_STACK_SIZE,
            NULL, (tskIDLE_PRIORITY + 2UL), (TaskHandle_t *) NULL);

#if USING_USB_CDC
    xTaskCreate(cdc_task, "CDC Task", configMINIMAL_STACK_SIZE * 2, NULL,
            (tskIDLE_PRIORITY + 1UL), (TaskHandle_t *) NULL);
#endif
    vTaskStartScheduler();

    while (1)
        ;

    return 0;
}

void setupHardware() {
    SystemCoreClockUpdate();
    Board_Init();

    ITM_init();

    Board_LED_Set(0, false);
}

/***** Task Definition *****/

void vCalibrateTask(void *vParameters) {
    xmotor->calibrate();
    ymotor->calibrate();
    vTaskDelete(NULL);
}

void vReceiveTask(void *vParameters) {
    GParser parser;

    while (1) {
        /* get GCode from mDraw */
#if USING_USB_CDC
        char buffer[RCV_BUFSIZE] = {'0'};
        int idx = 0;
        while (1) {
            int len = USB_receive((uint8_t *) (buffer + idx), RCV_BUFSIZE);

            char *pos = strstr((buffer + idx), "\n");// find '\n' <=> end of command

            if (pos != NULL)
                break;

            idx += len;
        }

        debugSerial.write(buffer);
        debugSerial.write((char *) "\r");
#else   // use usb debug
        char buffer[32] = {'0'};
        int idx = 0;
        int c;
        while (idx < 32) {
            if ((c = debugSerial.read()) != EOF) {
                buffer[idx++] = c;
                if (c == '\n')
                    break;
            }
        }
        ITM_write(buffer);
#endif

        /* parse GCode */
        Command gcode = parser.parse(buffer, strlen(buffer));

        /* send commands into queue */
        xQueueSendToBack(qCommand, &gcode, portMAX_DELAY);
    }
}

void vExecuteTask(void *vParameters) {

    Command recv;

    while (1) {
        /* wait for commands from queue */
        xQueueReceive(qCommand, &recv, portMAX_DELAY);

        /* do something useful*/
        switch (recv.type) {
        case Command::connected:
            debugSerial.write("M10 XY 380 310 0.00 0.00 A0 B0 H0 S80 U160 D90 \n");
            break;
        case Command::laser:
            // TODO laser

            break;
        case Command::move:
            /* calculate new rpm*/
            {
                float deltaX = recv.params[0] - xmotor->getCurrentPosition();
                float deltaY = recv.params[1] - ymotor->getCurrentPosition();

                float rpmX = abs(deltaX / deltaY) * motorBaseRpm;
                xmotor->setRpm(rpmX);
                ymotor->setRpm(motorBaseRpm);
            }
            /* move */
            xmotor->move(recv.params[0]);
            ymotor->move(recv.params[1]);

            break;
        case Command::pen_position:
            pen->moveServo(recv.params[0]);
            break;
        case Command::pen_setting:
            // ignore
            break;
        case Command::plotter_setting:
            // ignore
            break;
        case Command::to_origin:
            xmotor->setRpm(60);
            ymotor->setRpm(60);
            xmotor->move(0);
            ymotor->move(0);
            break;
        case Command::invalid:
        default:
            break;
        }

        /* send 'OK' back to mDraw */
#if USING_USB_CDC
        USB_send((uint8_t *) message, 4);
#else
        debugSerial.write("OK\n");
#endif

    }
}

/* the following is required if runtime statistics are to be collected */
extern "C" {

/* X axis motor */
void SCT2_IRQHandler(void) {
    portEND_SWITCHING_ISR(xmotor->irqHandler());
}

/* Y axis motor */
void SCT3_IRQHandler(void) {
    portEND_SWITCHING_ISR(ymotor->irqHandler());
}

void vConfigureTimerForRunTimeStats(void) {
    Chip_SCT_Init(LPC_SCTSMALL1);
    LPC_SCTSMALL1->CONFIG = SCT_CONFIG_32BIT_COUNTER;
    LPC_SCTSMALL1->CTRL_U = SCT_CTRL_PRE_L(255) | SCT_CTRL_CLRCTR_L; // set prescaler to 256 (255 + 1), and start timer
}

}
/* end runtime statistics collection */
