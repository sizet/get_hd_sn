// ©

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <endian.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h> 
#include <sys/ioctl.h>
#include <linux/hdreg.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>




#define DMSG(msg_fmt, msg_args...) \
    printf("%s(%04u): " msg_fmt "\n", __FILE__, __LINE__, ##msg_args)




// 紀錄 SCSI 回傳的序號的緩衝大小.
// http://www.staff.uni-mainz.de/tacke/scsi/SCSI2.html
// [Clause 8 - All device types].
//   [8.2.5 INQUIRY command].
//     [Table 44 - INQUIRY command].
//       <Allocation length> 的大小是 1byte, 所以直接使用最大長度 255.
#define SCSI_SN_BUFFER_SIZE 255

// 實際儲存序號的緩衝大小.
// 使用 SCSI_IOCTL_SEND_COMMAND 和 SG_IO 取得的序號最後並不會自動補上 '\0',
// 所以需要多 1byte 填充 '\0',
// 實際上傳給 <Allocation length> 的緩衝大小還是 SCSI_SN_BUFFER_SIZE.
#define STORE_SN_BUFFER_SIZE (SCSI_SN_BUFFER_SIZE + 1)




// SCSI INQUIRY 指令.
// http://www.staff.uni-mainz.de/tacke/scsi/SCSI2.html
// [Clause 8 - All device types].
//   [8.2.5 INQUIRY command].
//     [Table 44 - INQUIRY command].
struct scsi_inquiry_info
{
    unsigned char operation_code;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    unsigned char evpd:1,
                  reserved1:4,
                  logical_unit_number:3;
#elif __BYTE_ORDER == __BIG_ENDIAN
    unsigned char logical_unit_number:3,
                  reserved1:4,
                  evpd:1;
#else
#error "please check endian type"
#endif
    unsigned char page_code;
    unsigned char reserved2;
    unsigned char allocation_length;
    unsigned char control;
} __attribute__((packed));

// SCSI 回傳的序號的訊息格式.
// http://www.staff.uni-mainz.de/tacke/scsi/SCSI2.html
// [Clause 8 - All device types].
//   [8.3.4.5 Unit serial number page].
//     [Table 107 - Unit serial number page].
struct scsi_vpd_usn_info
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
    unsigned char peripheral_device_type:4,
                  peripheral_qualifier:4;
#elif __BYTE_ORDER == __BIG_ENDIAN
    unsigned char peripheral_qualifier:4,
                  peripheral_device_type:4;
#else
#error "please check endian type"
#endif
    unsigned char page_code;
    unsigned char reserved1;
    unsigned char page_length;
    unsigned char product_serial_number[];
} __attribute__((packed));

// 參考 linux/include/scsi/scsi_ioctl.h
// typedef struct scsi_ioctl_command.
struct scsi_ioctl_cmd
{
    unsigned int inlen;
    unsigned int outlen;
    unsigned char data[];
};




// 使用 ioctl(HDIO_GET_IDENTITY) 取得序號.
int use_hdio_get_identity(
    char *hd_path)
{
    int fret = -1, cret, disk_fd;
    struct hd_driveid disk_info;


    disk_fd = open(hd_path, O_RDONLY);
    if(disk_fd == -1)
    {
        DMSG("call open(%s) fail [%s]", hd_path, strerror(errno));
        goto FREE_01;
    }

    memset(&disk_info, 0, sizeof(disk_info));
    cret = ioctl(disk_fd, HDIO_GET_IDENTITY, &disk_info);
    if(cret == -1)
    {
        DMSG("call ioctl(HDIO_GET_IDENTITY) fail [%s]", strerror(errno));
        goto FREE_02;
    }

    DMSG("HDIO_GET_IDENTITY       : [%s]", disk_info.serial_no);

    fret = 0;
FREE_02:
    close(disk_fd);
FREE_01:
    return fret;
}

// 填充取得序號的 SCSI 指令.
void fill_scsi_get_sn_cmd(
    struct scsi_inquiry_info *scsi_cmd_buf)
{
    memset(scsi_cmd_buf, 0, sizeof(struct scsi_inquiry_info));

    // http://www.staff.uni-mainz.de/tacke/scsi/SCSI2.html
    // [Clause 8 - All device types].
    //   [8.2.5 INQUIRY command].
    //     [Table 44 - INQUIRY command].

    // <Operation code> = 0x12 (標準查詢).
    scsi_cmd_buf->operation_code = 0x12;

    // <EVPD> = 1 (要求 <Page code> 指定的資料).
    scsi_cmd_buf->evpd = 0x1;

    // <Page code> = 0x80 (要求取得序號).
    // [Clause 8 - All device types].
    //   [8.3.4 Vital product data parameters].
    //     [Table 102 - Vital product data page codes].
    //       [80h - Unit serial number page].
    scsi_cmd_buf->page_code = 0x80;

    // <Allocation length> = SCSI_SN_BUFFER_SIZE (提供的緩衝大小).
    // ** 實際測試此欄位似乎沒作用, 序號長度如果超過給定的值,
    // ** 則緩衝內紀錄的序號長度還是會超過給定的限制長度,
    // ** 實際限制應該是以傳送給 ioctl() 的參數所記錄的輸出緩衝大小為準.
    // ** SCSI_IOCTL_SEND_COMMAND 是 (struct scsi_ioctl_cmd).outlen
    // ** SG_IO 是 (struct sg_io_hdr).dxfer_len
    scsi_cmd_buf->allocation_length = SCSI_SN_BUFFER_SIZE;

    // <Control> = 0.
    // [Clause 7 - SCSI commands and status].
    //   [7.2.7 Control field].
    //     [Table 25 - Control field].
    scsi_cmd_buf->control = 0x0;
}

// 使用 ioctl(SCSI_IOCTL_SEND_COMMAND) 取得序號 (新版方法).
int use_scsi_ioctl_send_command(
    char *hd_path)
{
    int fret = -1, cret, disk_fd;
    struct scsi_ioctl_cmd *scsi_io_cmd;
    struct scsi_vpd_usn_info *usn_data;
    unsigned int idx;

    // SCSI 資料的緩衝,
    //
    // 在傳送 SCSI 指令時, SCSI 指令會紀錄在 (struct scsi_ioctl_cmd).data, 緩衝的內容會是 :
    // [4byte] (struct scsi_ioctl_cmd).inlen.
    // [4byte] (struct scsi_ioctl_cmd).outlen.
    // [6byte] struct scsi_inquiry_info (SCSI 指令).
    //
    // 在接收回傳的資料時, 資料會紀錄在 (struct scsi_ioctl_cmd).data, 緩衝的內容會是 :
    // [4byte] (struct scsi_ioctl_cmd).inlen.
    // [4byte] (struct scsi_ioctl_cmd).outlen.
    // [Nbyte] struct scsi_vpd_usn_info (回傳的序號訊息).
    unsigned char scsi_io_buf[sizeof(struct scsi_ioctl_cmd) +
                              sizeof(struct scsi_vpd_usn_info) + STORE_SN_BUFFER_SIZE];


    // 設定參數.
    // http://sg.danny.cz/sg/p/sg_v3_ho/ch08s26.html
    scsi_io_cmd = (struct scsi_ioctl_cmd *) scsi_io_buf;
    // 要寫入的資料長度.
    scsi_io_cmd->inlen = 0;
    // 儲存回傳的資料的緩衝大小.
    // 資料會從 scsi_io_cmd->data 開始寫入, 所以要扣掉 inlen 和 outlen 的大小.
    // -1 是預留 '\0'.
    scsi_io_cmd->outlen = (sizeof(scsi_io_buf) - sizeof(struct scsi_ioctl_cmd)) - 1;
    // 填充 SCSI 指令.
    fill_scsi_get_sn_cmd((struct scsi_inquiry_info *) scsi_io_cmd->data);

    disk_fd = open(hd_path, O_RDONLY);
    if(disk_fd == -1)
    {
        DMSG("call open(%s) fail [%s]", hd_path, strerror(errno));
        goto FREE_01;
    }

    cret = ioctl(disk_fd, SCSI_IOCTL_SEND_COMMAND, scsi_io_cmd);
    if(cret == -1)
    {
        DMSG("call ioctl(SCSI_IOCTL_SEND_COMMAND) fail [%s]", strerror(errno));
        goto FREE_02;
    }

    usn_data = (struct scsi_vpd_usn_info *) scsi_io_cmd->data;
    // 填充 '\0', usn_data->page_length 會紀錄實際的序號長度.
    idx = usn_data->page_length < SCSI_SN_BUFFER_SIZE ?
          usn_data->page_length : SCSI_SN_BUFFER_SIZE;
    usn_data->product_serial_number[idx] = '\0';

    DMSG("SCSI_IOCTL_SEND_COMMAND : [%s]", usn_data->product_serial_number);

    fret = 0;
FREE_02:
    close(disk_fd);
FREE_01:
    return fret;
}

// 使用 ioctl(SG_IO) 取得序號 (舊版方法).
int use_sg_io(
    char *hd_path)
{
    int fret = -1, cret, disk_fd;
    unsigned char usn_buf[sizeof(struct scsi_vpd_usn_info) + STORE_SN_BUFFER_SIZE];
    struct scsi_inquiry_info scsi_cmd;
    struct scsi_vpd_usn_info *usn_data;
    struct sg_io_hdr sg_info;
    unsigned int idx;


    // 填充 SCSI 指令.
    fill_scsi_get_sn_cmd(&scsi_cmd);

    // 設定參數.
    // http://sg.danny.cz/sg/p/sg3.txt
    memset(&sg_info, 0, sizeof(sg_info));
    // 'S' 表示 SCSI.
    sg_info.interface_id = 'S';
    // 表示從 SCSI 讀取資料.
    sg_info.dxfer_direction = SG_DXFER_FROM_DEV;
    // 回應超時.
    sg_info.timeout = 1000;
    // 指令的內容.
    sg_info.cmdp = (unsigned char *) &scsi_cmd;
    // 指令的長度.
    sg_info.cmd_len = sizeof(struct scsi_inquiry_info);
    // 儲存回傳的資料的緩衝.
    sg_info.dxferp = usn_buf;
    // 儲存回傳的資料的緩衝大小.
    // -1 是預留 '\0'.
    sg_info.dxfer_len = sizeof(usn_buf) - 1;

    disk_fd = open(hd_path, O_RDONLY);
    if(disk_fd == -1)
    {
        DMSG("call open(%s) fail [%s]", hd_path, strerror(errno));
        goto FREE_01;
    }

    cret = ioctl(disk_fd, SG_IO, &sg_info);
    if(cret == -1)
    {
        DMSG("call ioctl(SG_IO) fail [%s]", strerror(errno));
        goto FREE_02;
    }

    usn_data = (struct scsi_vpd_usn_info *) usn_buf;
    // 填充 '\0', usn_data->page_length 會紀錄實際的序號長度.
    idx = usn_data->page_length < SCSI_SN_BUFFER_SIZE ?
          usn_data->page_length : SCSI_SN_BUFFER_SIZE;
    usn_data->product_serial_number[idx] = '\0';

    DMSG("SG_IO                   : [%s]", usn_data->product_serial_number);

    fret = 0;
FREE_02:
    close(disk_fd);
FREE_01:
    return fret;
}

int main(
    int argc,
    char **argv)
{
    char *hd_path;


    if(argc != 2)
        goto FREE_HELP;

    hd_path = argv[1];

    if(use_hdio_get_identity(hd_path) < 0)
    {
        DMSG("call use_hdio_get_identity() fail");
    }

    if(use_scsi_ioctl_send_command(hd_path) < 0)
    {
        DMSG("call use_scsi_ioctl_send_command() fail");
    }

    if(use_sg_io(hd_path) < 0)
    {
        DMSG("call use_sg_io() fail");
    }

    return 0;
FREE_HELP:
    printf("\nget_disk_sn <hard disk device path (ex : /dev/sda)>\n\n");
    return 0;
}
