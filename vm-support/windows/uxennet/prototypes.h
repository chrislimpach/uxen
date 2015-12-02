NDIS_STATUS uxen_net_send_packet(Uxennet *n, PNDIS_PACKET p);
void uxen_net_free_adapter(Uxennet *n);
NTSTATUS uxen_net_init_adapter(Uxennet *n);
int uxen_net_recv_packet(MP_ADAPTER *adapter, uxen_v4v_ring_handle_t *rh);
void uxen_net_callback_worker(Uxennet *n);
NTSTATUS uxen_net_start_adapter(MP_ADAPTER *a);
NTSTATUS uxen_net_stop_adapter(MP_ADAPTER *a);
void NICSendQueuedPackets( MP_ADAPTER *adapter);
NTSTATUS platform_get_mac_address(IN PDEVICE_OBJECT pdo, UCHAR *mac_address);
NTSTATUS platform_get_mtu(IN PDEVICE_OBJECT pdo, ULONG *mtu);
