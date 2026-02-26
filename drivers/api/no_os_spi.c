
#include <inttypes.h>
#include "no_os_spi.h"
#include <stdlib.h>
#include "no_os_error.h"

/**
 * @brief Initialize the SPI communication peripheral.
 * @param desc - The SPI descriptor.
 * @param param - The structure that contains the SPI parameters.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t no_os_spi_init(struct no_os_spi_desc **desc,
		       const struct no_os_spi_init_param *param)
{
	int32_t ret;

	if (!param || !param->platform_ops)
		return -EINVAL;

	if (!param->platform_ops->init)
		return -ENOSYS;

	ret = param->platform_ops->init(desc, param);
	if (ret)
		return ret;

	(*desc)->platform_ops = param->platform_ops;
	(*desc)->parent = param->parent;

	return 0;
}

/**
 * @brief Free the resources allocated by no_os_spi_init().
 * @param desc - The SPI descriptor.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t no_os_spi_remove(struct no_os_spi_desc *desc)
{
	if (!desc || !desc->platform_ops)
		return -EINVAL;

	if (!desc->platform_ops->remove)
		return -ENOSYS;

	return desc->platform_ops->remove(desc);
}

/**
 * @brief Write and read data to/from SPI.
 * @param desc - The SPI descriptor.
 * @param data - The buffer with the transmitted/received data.
 * @param bytes_number - Number of bytes to write/read.
 * @return 0 in case of success, -1 otherwise.
 */
int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc,
				 uint8_t *data,
				 uint16_t bytes_number)
{
	if (!desc || !desc->platform_ops)
		return -EINVAL;

	if (!desc->platform_ops->write_and_read)
		return -ENOSYS;

	return desc->platform_ops->write_and_read(desc, data, bytes_number);
}

/**
 * @brief  Iterate over head list and send all spi messages
 * @param desc - The SPI descriptor.
 * @param msgs - Array of messages.
 * @param len - Number of messages in the array.
 * @return 0 in case of success, negativ error code otherwise.
 */
int32_t no_os_spi_transfer(struct no_os_spi_desc *desc,
			   struct no_os_spi_msg *msgs,
			   uint32_t len)
{
	int32_t  ret;
	uint32_t i;

	if (!desc || !desc->platform_ops)
		return -EINVAL;

	if (desc->platform_ops->transfer)
		return desc->platform_ops->transfer(desc, msgs, len);

	for (i = 0; i < len; i++) {
		if (msgs[i].rx_buff != msgs[i].tx_buff || !msgs[i].tx_buff)
			return -EINVAL;
		ret = no_os_spi_write_and_read(desc, msgs[i].rx_buff,
					       msgs[i].bytes_number);
		if (NO_OS_IS_ERR_VALUE(ret))
			return ret;
	}

	return 0;
}
