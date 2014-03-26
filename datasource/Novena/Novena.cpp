/*

Osqoop, an open source software oscilloscope.
Copyright (C) 2006--2009 Stephane Magnenat <stephane at magnenat dot net>
http://stephane.magnenat.net
Laboratory of Digital Systems
http://www.eig.ch/fr/laboratoires/systemes-numeriques/
Engineering School of Geneva
http://hepia.hesge.ch/
See authors file in source distribution for details about contributors



This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include <QtCore>
#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include "Novena.h"
#include <Novena.moc>

#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>

#define FAMILY_NAME "kosagi-fpga"
#define DATA_SIZE 4096

/* list of valid commands */
enum kosagi_fpga_commands {
        KOSAGI_CMD_UNSPEC,
        KOSAGI_CMD_SEND,
        KOSAGI_CMD_READ,
        __KOSAGI_CMD_MAX,
};

/* list of valid command attributes */
enum kosagi_fpga_attributes {
        KOSAGI_ATTR_NONE,
        KOSAGI_ATTR_FPGA_DATA,
        KOSAGI_ATTR_MESSAGE,
        __KOSAGI_ATTR_MAX,
};

//! Dialog box for choosing sound input
class NovenaDialog : public QDialog
{
public:
	QListWidget *soundInputsList; //!< the list holding available sound inputs

	//! Creates the widgets, especially the list of available data sources, which is filled from the dataSources parameter
	NovenaDialog(const QStringList &items)
	{
		QVBoxLayout *layout = new QVBoxLayout(this);

		layout->addWidget(new QLabel(tr("Please choose a sound input")));

		soundInputsList = new QListWidget();
		soundInputsList->addItems(items);
		layout->addWidget(soundInputsList);
		
		connect(soundInputsList, SIGNAL(itemDoubleClicked(QListWidgetItem *)), SLOT(accept()));
	}
};


	#include <fcntl.h>
	#include <sys/ioctl.h>
	#include <sys/soundcard.h>
	#include <unistd.h>
		
	class NovenaSystemSpecificData
	{
	private:
		struct nl_sock *handle;
		struct nl_cache *cache;
		struct genl_family *id;
		nlmsghdr *nhdr;
		void *bufferData;
		quint8 *bufferDataPtr;
		int bufferDataSize;

	public:
		NovenaSystemSpecificData() {
			handle = NULL;
			cache = NULL;
			id = NULL;
			nhdr = NULL;
			bufferData = NULL;
			bufferDataSize = 0;
		}
		
		~NovenaSystemSpecificData()
		{
			if (nhdr)
				free(nhdr);
		}
		
		bool openDevice()
		{
			int ret;

			handle = nl_socket_alloc();
			if (!handle) {
				fprintf(stderr, "Failed to allocate netlink handle\n");
				return false;
			}

			ret = genl_connect(handle);
			if (ret) {
				fprintf(stderr, "Failed to connect to generic netlink: %s\n",
						nl_geterror(ret));
				return false;
			}

			ret = genl_ctrl_alloc_cache(handle, &cache);
			if (ret) {
				fprintf(stderr, "Failed to allocate generic netlink cache\n");
				return false;
			}

			id = genl_ctrl_search_by_name(cache, FAMILY_NAME);
			if (!id) {
				fprintf(stderr, "Family %s not found\n", FAMILY_NAME);
				return false;
			}

			ret = nl_socket_set_msg_buf_size(handle, 2 * DATA_SIZE);
			if (ret < 0) {
				fprintf(stderr, "Failed to set buffer size: %s\n",
						nl_geterror(ret));
				return false;
			}

			nl_socket_disable_auto_ack(handle);

			return true;
		}

		struct nl_msg *allocMsg(int cmd)
		{
			struct nl_msg *msg;
			void *header;

			msg = nlmsg_alloc_size(2 * DATA_SIZE);
			if (!msg) {
				fprintf(stderr, "Unable to alloc nlmsg\n");
				return NULL;
			}

			header = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ,
					genl_family_get_id(id),
					0, NLM_F_REQUEST, cmd, 1);
			if (!header) {
				fprintf(stderr, "Unable to call genlmsg_put()\n");
				nlmsg_free(msg);
				return NULL;
			}

			return msg;
		}

		int sendReadRequest(void)
		{
			struct nl_msg *msg;
			int ret;

			msg = allocMsg(KOSAGI_CMD_READ);
			if (!msg)
				return -1;

			ret = nl_send_auto(handle, msg);
			if (ret < 0) {
				fprintf(stderr, "Unable to send msg: %s\n", nl_geterror(ret));
				nlmsg_free(msg);
				return ret;
			}
			nlmsg_free(msg);
			return 0;
		}

		int doReadRequest(void)
		{
			int ret;
			struct sockaddr_nl nla;
			struct genlmsghdr *ghdr;

			if (nhdr)
				free(nhdr);
			nhdr = NULL;

			ret = nl_recv(handle, &nla, (unsigned char **)&nhdr, NULL);
			if (ret < 0) {
				fprintf(stderr, "Unable to receive data: %s\n", nl_geterror(ret));
				nhdr = NULL;
				return -1;
			}

			ghdr = (genlmsghdr *)nlmsg_data((const nlmsghdr *)nhdr);
			bufferData = (quint32 *)genlmsg_user_data(ghdr, 0);
			bufferDataPtr = (quint8 *)bufferData;

			if (genlmsg_len(ghdr) != DATA_SIZE) {
				fprintf(stderr, "Warning: Wanted %d bytes, read %d bytes\n",
					DATA_SIZE, genlmsg_len(ghdr));
				nhdr = NULL;
				free(nhdr);
				return -1;
			}

			bufferDataSize = genlmsg_len(ghdr);
			return 0;
		}

		int getRawData(std::valarray<std::valarray<signed short> > *data)
		{
			int ret;
			quint8 *d;

			/* Read more data, if necessary */
			if (bufferDataSize < 512) {
				if (sendReadRequest())
					return 0;
				if (doReadRequest())
					return 0;
			}

			for (size_t sample = 0; sample < 512; ) {
				for (size_t channel = 0; channel < 1; channel++) {
					for (int byte = 0; byte < 8; byte++) {
						(*data)[channel][sample + byte] = *bufferDataPtr++;
					}
				}
				sample += 16;
			}
			bufferDataSize -= (512 * 2);

			return 0;
		}
	};


QString NovenaDataSourceDescription::name() const
{
	return "Novena capture";
}

QString NovenaDataSourceDescription::description() const
{
	return "Novena FPGA-based input capture";
}

DataSource *NovenaDataSourceDescription::create() const
{
	return new NovenaDataSource(this);
}


NovenaDataSource::NovenaDataSource(const DataSourceDescription *description) :
	DataSource(description)
{
	privateData  = new NovenaSystemSpecificData;
}

NovenaDataSource::~NovenaDataSource()
{
	delete privateData;
}

bool NovenaDataSource::init(void)
{
	return privateData->openDevice();
}

unsigned NovenaDataSource::getRawData(std::valarray<std::valarray<signed short> > *data)
{
	return privateData->getRawData(data);
}

unsigned NovenaDataSource::inputCount() const
{
	return 2;
}

unsigned NovenaDataSource::samplingRate() const
{
	return 44100;
}

unsigned NovenaDataSource::unitPerVoltCount() const
{
	return 10000;
}


Q_EXPORT_PLUGIN(NovenaDataSourceDescription)
