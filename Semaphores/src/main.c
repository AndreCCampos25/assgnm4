#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/pwm.h>
#include <drivers/adc.h>
#include <sys/printk.h>
#include <sys/__assert.h>
#include <timing/timing.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GPIO0_NID DT_NODELABEL(gpio0) 
#define PWM0_NID DT_NODELABEL(pwm0) 
#define BOARDLED_PIN 0x0e

/*ADC definitions and includes*/
#include <hal/nrf_saadc.h>
#define ADC_NID DT_NODELABEL(adc) 
#define ADC_RESOLUTION 10
#define ADC_GAIN ADC_GAIN_1_4
#define ADC_REFERENCE ADC_REF_VDD_1_4
#define ADC_ACQUISITION_TIME ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40)
#define ADC_CHANNEL_ID 1  

/* This is the actual nRF ANx input to use. Note that a channel can be assigned to any ANx. In fact a channel can */
/*    be assigned to two ANx, when differential reading is set (one ANx for the positive signal and the other one for the negative signal) */  
/* Note also that the configuration of differnt channels is completely independent (gain, resolution, ref voltage, ...) */
#define ADC_CHANNEL_INPUT NRF_SAADC_INPUT_AIN1 

#define BUFFER_SIZE 1

/* ADC channel configuration */
static const struct adc_channel_cfg my_channel_cfg = {
	.gain = ADC_GAIN,
	.reference = ADC_REFERENCE,
	.acquisition_time = ADC_ACQUISITION_TIME,
	.channel_id = ADC_CHANNEL_ID,
	.input_positive = ADC_CHANNEL_INPUT
};

/* Global vars */
struct k_timer my_timer;
const struct device *adc_dev = NULL;
static uint16_t adc_sample_buffer[BUFFER_SIZE];

int array[10];
int array_final_10[10];
int ctrl_10=0;

/* Takes one sample */
static int adc_sample(void)
{
	int ret;
	const struct adc_sequence sequence = {
		.channels = BIT(ADC_CHANNEL_ID),
		.buffer = adc_sample_buffer,
		.buffer_size = sizeof(adc_sample_buffer),
		.resolution = ADC_RESOLUTION,
	};

	if (adc_dev == NULL) {
            printk("adc_sample(): error, must bind to adc first \n\r");
            return -1;
	}

	ret = adc_read(adc_dev, &sequence);
	if (ret) {
            printk("adc_read() failed with code %d\n", ret);
	}	

	return ret;
}

//#######################################################

/* Size of stack area used by each thread (can be thread specific, if necessary)*/
#define STACK_SIZE 1024

/* Thread scheduling priority */
#define thread_ADC_prio 1
#define thread_FILTRO_prio 1
#define thread_PWM_prio 1

/* Therad periodicity (in ms)*/
#define thread_ADC_period 1000


/* Create thread stack space */
K_THREAD_STACK_DEFINE(thread_ADC_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_FILTRO_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_PWM_stack, STACK_SIZE);
  
/* Create variables for thread data */
struct k_thread thread_ADC_data;
struct k_thread thread_FILTRO_data;
struct k_thread thread_PWM_data;

/* Create task IDs */
k_tid_t thread_ADC_tid;
k_tid_t thread_FILTRO_tid;
k_tid_t thread_PWM_tid;

/* Global vars (shared memory between tasks A/B and B/C, resp) */
int val_1 = 0;
int media_final = 0;

/* Semaphores for task synch */
struct k_sem sem_val_1;
struct k_sem sem_media_final;

/* Thread code prototypes */
void thread_ADC_code(void *argA, void *argB, void *argC);
void thread_FILTRO_code(void *argA, void *argB, void *argC);
void thread_PWM_code(void *argA, void *argB, void *argC);

//#######################################################

void main(void)
{
     /* Create and init semaphores */
    k_sem_init(&sem_val_1, 0, 1);
    k_sem_init(&sem_media_final, 0, 1);
    
    /* Create tasks */
    thread_ADC_tid = k_thread_create(&thread_ADC_data, thread_ADC_stack,
        K_THREAD_STACK_SIZEOF(thread_ADC_stack), thread_ADC_code,
        NULL, NULL, NULL, thread_ADC_prio, 0, K_NO_WAIT);

    thread_FILTRO_tid = k_thread_create(&thread_FILTRO_data, thread_FILTRO_stack,
        K_THREAD_STACK_SIZEOF(thread_FILTRO_stack), thread_FILTRO_code,
        NULL, NULL, NULL, thread_FILTRO_prio, 0, K_NO_WAIT);

    thread_FILTRO_tid = k_thread_create(&thread_PWM_data, thread_PWM_stack,
        K_THREAD_STACK_SIZEOF(thread_PWM_stack), thread_PWM_code,
        NULL, NULL, NULL, thread_PWM_prio, 0, K_NO_WAIT); 

    return;
}

/* Thread code implementation */
void thread_ADC_code(void *argA , void *argB, void *argC)
{
  /* Timing variables to control task periodicity */
    int64_t fin_time=0, release_time=0;

    /* Other variables */
    long int nact = 0;
    
    printk("Thread A init (periodic)\n");

//#######################################################

    int err=0;

    /* Welcome message */
    printk("\n\r Simple adc demo for  \n\r");
    printk(" Reads an analog input connected to AN%d and prints its raw and mV value \n\r", ADC_CHANNEL_ID);
    printk(" *** ASSURE THAT ANx IS BETWEEN [0...3V]\n\r");
         
    /* ADC setup: bind and initialize */
    adc_dev = device_get_binding(DT_LABEL(ADC_NID));
	if (!adc_dev) {
        printk("ADC device_get_binding() failed\n");
    } 
    err = adc_channel_setup(adc_dev, &my_channel_cfg);
    if (err) {
        printk("adc_channel_setup() failed with error code %d\n", err);
    }
    
    /* It is recommended to calibrate the SAADC at least once before use, and whenever the ambient temperature has changed by more than 10 °C */
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
 
 //#######################################################
    

    /* Compute next release instant */
    release_time = k_uptime_get() + thread_ADC_period;

    while(1) {
//#######################################################
        err=adc_sample();
        val_1=(uint16_t)(1000*adc_sample_buffer[0]*((float)3/1023));
        
        if(err) 
        {
            printk("adc_sample() failed with error code %d\n\r",err);
        }
        else 
        {
            if(adc_sample_buffer[0] > 1023) 
            {
                printk("adc reading out of range\n\r");
            }
            else 
            {
                /* ADC is set to use gain of 1/4 and reference VDD/4, so input range is 0...VDD (3 V), with 10 bit resolution */
                printk("adc reading: raw:%4u / %4u mV: \n\r",adc_sample_buffer[0],(uint16_t)(1000*adc_sample_buffer[0]*((float)3/1023)));
            }
        }
//#######################################################
        
        /* Do the workload */          
        printk("\n\nThread A instance %ld released at time: %lld (ms). \n",++nact, k_uptime_get());    

        k_sem_give(&sem_val_1);

       
        /* Wait for next release instant */ 
        fin_time = k_uptime_get();
        if( fin_time < release_time) {
            k_msleep(release_time - fin_time);
            release_time += thread_ADC_period;

        }
    }

}

void thread_FILTRO_code(void *argA , void *argB, void *argC)
{
    /* Other variables */
    long int nact = 0;

    printk("Thread B init (sporadic, waits on a semaphore by task A)\n");
    while(1) {
        k_sem_take(&sem_val_1,  K_FOREVER);
        printk("Thread B instance %ld released at time: %lld (ms). \n",++nact, k_uptime_get());  
        
        /* Get one sample, checks for errors and prints the values */

        /*if(ctrl_10<10)
        {
          array[ctrl_10]=val_1;
        }
        else if(ctrl_10==10)
        {
          for(int i=0;i<10;i++)
          {
            sum+=array[i];
            media=sum/10;
          }
          desvio=media*0.1;
        }

        else
        {
          for(int j=0;j<10;j++)
          {
            if(array[j]>=(media-desvio) && array[j]<=(media+desvio))
            {
              array_final_10[ctrl_final_10]=array[j];
              ctrl_final_10++;
            }
          }
          
          while(ctrl_final_10<10)
          {
            array[l]=val_1;
            l++;
            if(l>=10)
            {
              l=0;
            }
            
            for(int i=0;i<10;i++)
            {
             sum+=array[i];
             media=sum/10;
            }

            double desvio=media*0.1;

            for(int j=0;j<10;j++)
            {
              if(array[j]>=(media-desvio) && array[j]<=(media+desvio))
              {
                array_final_10[ctrl_final_10]=array[j];
                ctrl_final_10++;
              }
            }
          }

          if(ctrl_final_10>=10)
          {
            array[l]=val_1;
            l++;
            if(l>=10)
            {
              l=0;
            }
            
            for(int i=0;i<10;i++)
            {
             sum+=array[i];
             media=sum/10;
            }

            double desvio=media*0.1;

            for(int j=0;j<10;j++)
            {
              if(array[j]>=(media-desvio) && array[j]<=(media+desvio))
              {
                array_final_10[x]=array[j];
                x++;
                if(x>=10)
                {
                 x=0;
                }
                for(int i=0;i<10;i++)
                {
                  sum+=array_final_10[i];
                  media_final=sum/10;
                }
              }
            }

          }
        
        ctrl_10++;
        */

        k_sem_give(&sem_media_final);

  }
}


void thread_PWM_code(void *argA , void *argB, void *argC)
{
    /* Other variables */
    long int nact = 0;
    
    const struct device *pwm0_dev;          /* Pointer to PWM device structure */
    int ret_pwm=0;                          /* Generic return value variable */
    
    unsigned int pwmPeriod_us = 1000;       /* PWM priod in us */
    unsigned int val_duty=0;

    pwm0_dev = device_get_binding(DT_LABEL(PWM0_NID));
    if (pwm0_dev == NULL) {
	printk("Error: Failed to bind to PWM0\n r");
	return;
    }
    else  {
        printk("Bind to PWM0 successfull\n\r");            
    }

    printk("Thread C init (sporadic, waits on a semaphore by task A)\n");
    while(1) {
        k_sem_take(&sem_media_final, K_FOREVER);
        printk("Thread C instance %5ld released at time: %lld (ms). \n",++nact, k_uptime_get());          

         val_duty=((uint16_t)(1000*adc_sample_buffer[0]*((float)3/1023))*100)/3000;
        printk("PWM DC value set to %u %%\n\r",val_duty);
        
        ret_pwm = pwm_pin_set_usec(pwm0_dev, BOARDLED_PIN,
          pwmPeriod_us,val_duty, PWM_POLARITY_NORMAL);
       /* if (ret_pwm) 
        {
            printk("Error %d: failed to set pulse width\n", ret_pwm);
            return;
        }*/
        
  }
}






    /* Main loop */
    /*int sum=0;
    int media=0;
    int media_final=0;
    double desvio=0;
    int ctrl_final_10=0;
    int l=0;
    int x=0;*/
