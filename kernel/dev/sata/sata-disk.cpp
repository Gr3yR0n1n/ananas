#include <ananas/types.h>
#include <ananas/error.h>
#include "kernel/dev/ata.h"
#include "kernel/dev/sata.h"
#include "kernel/bio.h"
#include "kernel/device.h"
#include "kernel/driver.h"
#include "kernel/endian.h"
#include "kernel/lib.h"
#include "kernel/mbr.h"
#include "kernel/mm.h"
#include "kernel/trace.h"

#include "../ahci/ahci.h" // XXX

TRACE_SETUP;

namespace {

class SATADisk : public Ananas::Device, private Ananas::IDeviceOperations, private Ananas::IBIODeviceOperations
{
public:
	using Device::Device;
	virtual ~SATADisk() = default;

	IDeviceOperations& GetDeviceOperations() override
	{
		return *this;
	}

	IBIODeviceOperations* GetBIODeviceOperations() override
	{
		return this;
	}

	errorcode_t Attach() override;
	errorcode_t Detach() override;

	errorcode_t ReadBIO(struct BIO& bio) override;
	errorcode_t WriteBIO(struct BIO& bio) override;

	void Execute(struct SATA_REQUEST& sr);


private:
	struct ATA_IDENTIFY sd_identify;
	uint64_t sd_size;	/* in sectors */
	uint32_t sd_flags;
#define SATADISK_FLAGS_LBA48 1
};

void
SATADisk::Execute(struct SATA_REQUEST& sr)
{
	// XXX this is a hack
	auto p = static_cast<Ananas::AHCI::Port*>(d_Parent);
	p->Enqueue(&sr);
	p->Start();
#if 0
	dev->parent->driver->drv_enqueue(dev->parent, &sr);
	dev->parent->driver->drv_start(dev->parent);
#endif
}

errorcode_t
SATADisk::Attach()
{
	/*
	 * Once we are here, we know we're attaching to a SATA disk-device; we don't
	 * know anything else though, so we'll have to issue an IDENTIFY command to
	 * get down to the details.
	 */
	semaphore_t sem;
	sem_init(&sem, 0);

	struct SATA_REQUEST sr;
	memset(&sr, 0, sizeof(sr));
	sata_fis_h2d_make_cmd(&sr.sr_fis.fis_h2d, ATA_CMD_IDENTIFY);
	sr.sr_fis_length = 20;
	sr.sr_semaphore = &sem;
	sr.sr_count = 512;
	sr.sr_buffer = &sd_identify;
	sr.sr_flags = SATA_REQUEST_FLAG_READ;
	Execute(sr);

	/* Wait until the request has been completed */
	sem_wait(&sem);

	/* Fix endianness */
	auto p = reinterpret_cast<uint16_t*>(&sd_identify);
	for (unsigned int n = 0; n < sizeof(sd_identify) / 2; n++, p++)
		*p = betoh16(*p);

	/* Calculate the length of the disk */
	sd_size = ATA_GET_DWORD(sd_identify.lba_sectors);
	if (ATA_GET_WORD(sd_identify.features2) & ATA_FEAT2_LBA48) {
		sd_size  = ATA_GET_QWORD(sd_identify.lba48_sectors);
		sd_flags |= SATADISK_FLAGS_LBA48;
	}

	/* Terminate the model name */
	for(int n = sizeof(sd_identify.model) - 1; n > 0 && sd_identify.model[n] == ' '; n--)
		sd_identify.model[n] = '\0';

	Printf("<%s> - %u MB",
	 sd_identify.model,
 	 sd_size / ((1024UL * 1024UL) / 512UL));

	/*
	 * Read the first sector and pass it to the MBR code; this is crude
	 * and does not really belong here.
	 */
	struct BIO* bio = bio_read(this, 0, BIO_SECTOR_SIZE);
	if (BIO_IS_ERROR(bio))
		return ANANAS_ERROR(IO); /* XXX should get error from bio */

	mbr_process(this, bio);
	bio_free(bio);
	return ANANAS_ERROR(IO);
}

errorcode_t
SATADisk::Detach()
{
	return ananas_success();
}

errorcode_t
SATADisk::ReadBIO(struct BIO& bio)
{
	KASSERT(bio.length > 0, "invalid length");
	KASSERT(bio.length % 512 == 0, "invalid length"); /* XXX */

	struct SATA_REQUEST sr;
	memset(&sr, 0, sizeof(sr));
	/* XXX  we shouldn't always use lba-48 */
	sata_fis_h2d_make_cmd_lba48(&sr.sr_fis.fis_h2d, ATA_CMD_DMA_READ_EXT, bio.io_block, bio.length / BIO_SECTOR_SIZE);
	sr.sr_fis_length = 20;
	sr.sr_count = bio.length;
	sr.sr_bio = &bio;
	sr.sr_flags = SATA_REQUEST_FLAG_READ;
	Execute(sr);
	return ananas_success();
}

errorcode_t
SATADisk::WriteBIO(struct BIO& bio)
{
	struct SATA_REQUEST sr;
	memset(&sr, 0, sizeof(sr));
	/* XXX  we shouldn't always use lba-48 */
	sata_fis_h2d_make_cmd_lba48(&sr.sr_fis.fis_h2d, ATA_CMD_DMA_WRITE_EXT, bio.io_block, bio.length / BIO_SECTOR_SIZE);
	sr.sr_fis_length = 20;
	sr.sr_count = bio.length;
	sr.sr_bio = &bio;
	sr.sr_flags = SATA_REQUEST_FLAG_WRITE;
	Execute(sr);
	return ananas_success();
}

struct SATADisk_Driver : public Ananas::Driver
{
	SATADisk_Driver()
	 : Driver("satadisk")
	{
	}

	const char* GetBussesToProbeOn() const override
	{
		return nullptr; // instantiated by ahci-port
	}

	Ananas::Device* CreateDevice(const Ananas::CreateDeviceProperties& cdp) override
	{
		return new SATADisk(cdp);
	}
};

} // unnamed namespace

REGISTER_DRIVER(SATADisk_Driver)

/* vim:set ts=2 sw=2: */
