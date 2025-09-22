#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define DRIVER_NAME "lcd_screen"
#define NAME "lcd"
#define BUF_LEN 512
#define WQ_NAME "print_wq"
#define MAX_RETRIES 15
#define MAX_CUSTOM_CHARS 8

#define PIN_RS 0x01
#define PIN_EN 0x04
#define PIN_BL 0x08

typedef struct custom_char{
    int cg_memory_slot;
    uint8_t letter_design[8]; 
    char letter[5];
}custom_char;

static custom_char custom_chars[MAX_CUSTOM_CHARS] = {0};
static int custom_char_count = 0;
static const char *lt_letters = "ąĄčČęĘėĖįĮšŠųŲūŪžŽ";

static uint8_t letters[18][8] = {
    {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0e, 0x01}, //ą
    {0x0e, 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x02}, //Ą
    {0x0a, 0x04, 0x0e, 0x10, 0x10, 0x10, 0x0e, 0x00}, //č
    {0x0a, 0x04, 0x0e, 0x10, 0x10, 0x10, 0x0e, 0x00}, //č
    {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e, 0x01}, //ę
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f, 0x01}, //Ę
    {0x04, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e, 0x00}, //ė
    {0x04, 0x00, 0x1f, 0x10, 0x1e, 0x10, 0x1f, 0x00}, //Ė
    {0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02}, //į
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02}, //Į
    {0x0a, 0x04, 0x0e, 0x10, 0x0e, 0x01, 0x0e, 0x00}, //š
    {0x0a, 0x04, 0x0e, 0x10, 0x0e, 0x01, 0x0e, 0x00}, //š
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x01}, //ų
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x01}, //Ų
    {0x0e, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x00}, //ū
    {0x0e, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x00}, //ū
    {0x0a, 0x04 ,0x1f, 0x02, 0x04, 0x08, 0x1f, 0x00}, //ž
    {0x0a, 0x04 ,0x1f, 0x02, 0x04, 0x08, 0x1f, 0x00} //ž
};

static bool finished_work = true;

static char message[BUF_LEN];

static struct workqueue_struct *workqueue;
struct work_struct work;
void workqueue_print_message(struct work_struct *work);

static struct i2c_client *lcd_client;

static int expanderWrite(uint8_t data)
{
    return i2c_smbus_write_byte(lcd_client, data);
}

static int pulseEnable(uint8_t data)
{
    int ret = expanderWrite(data | PIN_EN);
    if (ret >= 0) {
        usleep_range(450, 1000);
        expanderWrite(data & ~PIN_EN);
        usleep_range(450, 1000);
    }
    return ret;
}

static int lcdWrite4(uint8_t nibble, uint8_t mode)
{
    uint8_t data = (nibble & 0xF0) | PIN_BL | mode;
    int ret = expanderWrite(data);
    if (ret < 0)
        return ret;
    return pulseEnable(data);
}

static int lcdSend(uint8_t value, uint8_t mode)
{
    int ret = lcdWrite4(value & 0xF0, mode);
    if (ret < 0)
        return ret;
    return lcdWrite4((value << 4) & 0xF0, mode);
}

static int lcdCommand(uint8_t cmd)
{
    int ret = lcdSend(cmd, 0);
    if (ret < 0)
        return ret;
    if (cmd == 0x01 || cmd == 0x02)
        usleep_range(2000, 3000); 
    return ret;
}

static int lcdData(uint8_t data)
{
    return lcdSend(data, PIN_RS);
}

static int lcdInit(void)
{
    int ret = 0;
    msleep(50);

    for (int i = 0; i < 3; i++) {
        ret = lcdWrite4(0x30, 0);
        if (ret < 0)
            return ret;
        if (i != 2)
            msleep(5);
    }

    udelay(150);
    ret = lcdWrite4(0x20, 0);
    if (ret < 0)
        return ret;

    if ((ret = lcdCommand(0x28)) < 0) return ret;
    if ((ret = lcdCommand(0x0C)) < 0) return ret;
    if ((ret = lcdCommand(0x06)) < 0) return ret;
    if ((ret = lcdCommand(0x01)) < 0) return ret;

    msleep(2);
    return 0;
}

static int lcdSetCursor(int row, int col)
{
    static const int row_offsets[] = {0x00, 0x40};
    return lcdCommand(0x80 | (col + row_offsets[row]));
}
static int find_lt_letter_index(const char *letter){
    for(int i=0;i<36;i++){
        if(strncmp(letter, &lt_letters[i], 2) == 0){
            if(i != 0)
                return i/2;
            return i;
        }
    }
    return -1;
}

static int write_to_cgram(void){
    for(int i=0;i<custom_char_count;i++){
        if(lcdCommand(0x40 | (custom_chars[i].cg_memory_slot<<3))<0)
            return -1;
        for(int j=0;j<8;j++){
            if(lcdData(custom_chars[i].letter_design[j])<0)
                return -1;
        }
    }
    return 0;
}

static int load_custom_chars(const char *msg){

    const char *p = msg;
    while(*p){
        int char_len = 1;
        if ((p[0] & 0xE0) == 0xC0) char_len = 2;
        else if ((p[0] & 0xF0) == 0xE0) char_len = 3;
        else if ((p[0] & 0xF8) == 0xF0) char_len = 4;

        int ind = find_lt_letter_index(p);
        pr_info("index: %d", ind);
        if(ind>=0) {
            int exists = 0;
            for(int i=0;i<custom_char_count;i++){
                if(strncmp(custom_chars[i].letter, p, char_len)==0){
                    exists = 1;
                    break;
                }
            }
            if(!exists){
                if(custom_char_count == MAX_CUSTOM_CHARS){
                    pr_warn("Too many custom letters");
                    break;
                }
                memcpy(custom_chars[custom_char_count].letter, p, char_len);
                memcpy(custom_chars[custom_char_count].letter_design, letters[ind], 8);
                custom_chars[custom_char_count].cg_memory_slot = custom_char_count;
                custom_char_count++;
            }
        }
        p+= char_len;
    }
    if(write_to_cgram() < 0)
        return -1;
    return 0;
}

static int lcdPrint(const char *s)
{
    int retries = 0;
    while (*s) {
        int char_len = 1;

        if ((s[0] & 0xE0) == 0xC0) char_len = 2;
        else if ((s[0] & 0xF0) == 0xE0) char_len = 3;
        else if ((s[0] & 0xF8) == 0xF0) char_len = 4;

        int ind = -1;

        for(int i=0;i<custom_char_count;i++){
            if(strncmp(custom_chars[i].letter, s, char_len) == 0){
                ind = custom_chars[i].cg_memory_slot;
                break;
            }
        }
        int lcd_char = (ind >=0) ? ind:s[0];

        if (lcdData(lcd_char) < 0) {
            if (++retries > 5)
                return -1;
        } else {
            s += char_len;
            retries = 0;
        }
    }
    return 0;
}

void workqueue_print_message(struct work_struct *work)
{
    finished_work = false;
    char *msg = kmalloc(512, GFP_KERNEL);
    if(!msg){
        finished_work = true;
        kfree(msg);
        return;
    }
    strncpy(msg, message, sizeof(message)-1);
    memset(message, 0, 511);
    int msg_len = strlen(msg);
    char *line1 = kmalloc(17, GFP_KERNEL);
    char *line2 = kmalloc(17, GFP_KERNEL);

    if (!line1 || !line2) {
        pr_err("Memory allocation failed\n");
        kfree(line1);
        kfree(line2);
        kfree(msg);
        finished_work = true;
        return;
    }

    int max_scroll = (msg_len > 32) ? (msg_len - 32 + 1) : 1;

    for (int i = 0; i < max_scroll; i++) {
        if (lcdCommand(0x01) < 0) {
            pr_err("Unable to clear screen\n");
            break;
        }
        msleep(2);

        custom_char_count = 0;
        memset(custom_chars, 0, sizeof(custom_chars));

        memset(line1, ' ', 16);
        if(strlen(msg)>16)
            strncpy(line1, msg + i, min(16, msg_len - i));
        else 
            strncpy(line1, msg + i, min(16, msg_len - i)-1);

        if(load_custom_chars(line1)<0){
            pr_err("Failed to load custom chars\n");
            break;
        }

        if (lcdSetCursor(0, 0) < 0 || lcdPrint(line1) < 0) {
            pr_err("Unable to print line 1\n");
            break;
        }
        memset(line2, ' ', 16);
        if (msg_len > i + 16){

            strncpy(line2, msg + i + 16, min(16, msg_len - (i + 16))-1);

            if(load_custom_chars(line2)<0){
                pr_err("Failed to load custom chars\n");
                break;
            }
            if (lcdSetCursor(1, 0) < 0 || lcdPrint(line2) < 0) {
                pr_err("Unable to print line 2\n");
                break;
            }
        }

        msleep(300);
    }

    kfree(line1);
    kfree(line2);
    kfree(msg);
    finished_work = true;
}


static ssize_t device_write(struct file *filp, const char __user *buffer,
                            size_t len, loff_t *offset)
{
    char msg[BUF_LEN];
    size_t copy_len = min(len, (size_t)(BUF_LEN - 1));

    if (!finished_work)
        return -EBUSY;

    if (copy_from_user(msg, buffer, copy_len))
        return -EFAULT;

    msg[copy_len] = '\0';

    if (strlen(msg) > 0) {
        strncpy(message, msg, BUF_LEN - 1);
        finished_work = false;
        queue_work(workqueue, &work);
    }

    return copy_len;
}


static int major_num;
static struct class *cls;

static struct file_operations fops = {
    .write = device_write,
};

static int lcd_screen_probe(struct i2c_client *client)
{
    int retry = 0;
    lcd_client = client;

    while (retry < MAX_RETRIES){
        if(lcdInit() >= 0)
            break;
        msleep(200);
        retry++;
    }
    if(retry == MAX_RETRIES){
        dev_err(&client->dev, "LCD init failed after %d retries\n", MAX_RETRIES);
        return -ENODEV;
    }

    major_num = register_chrdev(0, NAME, &fops);
    if (major_num < 0) {
        dev_err(&client->dev, "Failed to register character device\n");
        return major_num;
    }

    cls = class_create(NAME);
    device_create(cls, NULL, MKDEV(major_num, 0), NULL, NAME);

    dev_info(&client->dev, "LCD device created at /dev/%s\n", NAME);

    workqueue = create_singlethread_workqueue(WQ_NAME);
    if(workqueue == NULL){
        dev_err(&client->dev, "Failed to create workqueue\n");
        return -ENOMEM;
    }
    INIT_WORK(&work, workqueue_print_message);

    return 0;
}

static void lcd_screen_remove(struct i2c_client *client)
{   
    flush_workqueue(workqueue);
    destroy_workqueue(workqueue);
    device_destroy(cls, MKDEV(major_num, 0));
    class_destroy(cls);
    unregister_chrdev(major_num, NAME);
    dev_info(&client->dev, "LCD driver removed\n");
}

static const struct i2c_device_id lcd_screen_id[] = {
    { "lcd-screen", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, lcd_screen_id);

static const struct of_device_id lcd_screen_of_match[] = {
    { .compatible = "lcdvendor,lcd-screen" },
    { }
};
MODULE_DEVICE_TABLE(of, lcd_screen_of_match);

static struct i2c_driver lcd_screen_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = lcd_screen_of_match,
    },
    .probe = lcd_screen_probe,
    .remove = lcd_screen_remove,
    .id_table = lcd_screen_id,
};

module_i2c_driver(lcd_screen_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Erikas");
MODULE_DESCRIPTION("LCD screen driver");
