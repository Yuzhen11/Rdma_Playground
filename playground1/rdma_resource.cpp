# include "rdma_resource.hpp"

struct config_t rdma_config = {
  NULL,                         /* dev_name */
  NULL,                         /* server_name */
  19875,                        /* tcp_port */
  1,                            /* ib_port */
  -1                            /* gid_idx */
};

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t
htonll (uint64_t x)
{
  return bswap_64 (x);
}

static inline uint64_t
ntohll (uint64_t x)
{
  return bswap_64 (x);
}
#elif __BYTE_ORDER == __BIG_ENDIAN

static inline uint64_t
htonll (uint64_t x)
{
  return x;
}

static inline uint64_t
ntohll (uint64_t x)
{
  return x;
}
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

/* ------------------- generate local connection data --------------------- */

static struct cm_con_data_t
get_local_con_data(struct QP *res)
{
  struct cm_con_data_t local_con_data;
  union ibv_gid my_gid;
  int rc;
  if (rdma_config.gid_idx >= 0)
    {
      rc =
        ibv_query_gid (res->dev->ib_ctx, rdma_config.ib_port, rdma_config.gid_idx, &my_gid);
      if (rc)
        {
          fprintf (stderr, "could not get gid for port %d, index %d\n",
                   rdma_config.ib_port, rdma_config.gid_idx);
          assert(false);
        }
    }
  else
    memset (&my_gid, 0, sizeof my_gid);

  local_con_data.addr = htonll ((uintptr_t) res->dev->buf);
  local_con_data.rkey = htonl (res->mr->rkey);
  local_con_data.qp_num = htonl (res->qp->qp_num);
  local_con_data.lid = htons (res->dev->port_attr.lid);
  memcpy (local_con_data.gid, &my_gid, 16);
  //  fprintf (stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
  return local_con_data;
}

/* -------------------- dev_resources_init, dev_resources_create ----------------- */

// initialize res
static void
dev_resources_init (struct dev_resource *res)
{
  memset (res, 0, sizeof *res);
}

// res, buf must be initialized!
static int
dev_resources_create (struct dev_resource *res,char* buf,uint64_t size)
{
  struct ibv_device **dev_list = NULL;
  struct ibv_qp_init_attr qp_init_attr;
  struct ibv_device *ib_dev = NULL;
  int i;
  int num_devices;
  int mr_flags = 0;
  int rc = 0;

  dev_list = ibv_get_device_list (&num_devices);
  if (!dev_list)
    {
      fprintf (stderr, "failed to get IB devices list\n");
      rc = 1;
      goto dev_resources_create_exit;
    }
  /* if there isn't any IB device in host */
  if (!num_devices)
    {
      fprintf (stderr, "found %d device(s)\n", num_devices);
      rc = 1;
      goto dev_resources_create_exit;
    }
  //  fprintf (stdout, "found %d device(s)\n", num_devices);
  /* search for the specific device we want to work with */
  for (i = 0; i < num_devices; i++)
    {
      if (!rdma_config.dev_name)
        {
          rdma_config.dev_name = strdup (ibv_get_device_name (dev_list[i]));
          fprintf (stdout,
                   "device not specified, using first one found: %s\n",
                   rdma_config.dev_name);
        }
      if (!strcmp (ibv_get_device_name (dev_list[i]), rdma_config.dev_name))
        {
          ib_dev = dev_list[i];
          break;
        }
    }
  /* if the device wasn't found in host */
  if (!ib_dev)
    {
      fprintf (stderr, "IB device %s wasn't found\n", rdma_config.dev_name);
      rc = 1;
      goto dev_resources_create_exit;
    }
  /* get device handle */
  res->ib_ctx = ibv_open_device (ib_dev);

  if (!res->ib_ctx)
    {
      fprintf (stderr, "failed to open device %s\n", rdma_config.dev_name);
      rc = 1;
      goto dev_resources_create_exit;
    }

  //check the atomicity level for rdma operation
  int ret;
  ret = ibv_query_device(res->ib_ctx,&(res->device_attr));
  if (ret) {
    fprintf(stderr,"ibv quert device %d\n",ret);
    assert(false);
  }

  fprintf(stdout,"The max size can reg: %ld\n",res->device_attr.max_mr_size);

  switch(res->device_attr.atomic_cap) {

  case IBV_ATOMIC_NONE:
    fprintf(stdout,"atomic none\n");
    break;
  case IBV_ATOMIC_HCA:
    fprintf(stdout,"atmoic within device\n");
    break;
  case IBV_ATOMIC_GLOB:
    fprintf(stdout,"atomic globally\n");
    break;
  default:
    fprintf(stdout,"atomic unknown !!\n");
    assert(false);
  }

  /* We are now done with device list, free it */
  ibv_free_device_list (dev_list);
  dev_list = NULL;
  ib_dev = NULL;
  /* query port properties */
  if (ibv_query_port (res->ib_ctx, rdma_config.ib_port, &res->port_attr))
    {
      fprintf (stderr, "ibv_query_port on port %u failed\n", rdma_config.ib_port);
      rc = 1;
      goto dev_resources_create_exit;
    }

  /* allocate Protection Domain */
  res->pd = ibv_alloc_pd (res->ib_ctx);
  if (!res->pd)
    {
      fprintf (stderr, "ibv_alloc_pd failed\n");
      rc = 1;
      goto dev_resources_create_exit;
    }

  res->buf=buf;
  assert(buf != NULL);
  //  memset (res->buf, 0, size);//TODO!!!

  /* register the memory buffer */
  mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
    IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;//add cmp op

  fprintf(stdout,"registering memory\n");
  res->mr = ibv_reg_mr (res->pd, res->buf, size, mr_flags);
  if (!res->mr)
    {
      fprintf (stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
      rc = 1;
      goto dev_resources_create_exit;
    }
  fprintf (stdout,
	   "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
	   res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

 dev_resources_create_exit:
  if (rc)
    {
      /* Error encountered, cleanup */
      if (res->mr)
        {
          ibv_dereg_mr (res->mr);
          res->mr = NULL;
        }
      if (res->pd)
        {
          ibv_dealloc_pd (res->pd);
          res->pd = NULL;
        }
      if (res->ib_ctx)
        {
          ibv_close_device (res->ib_ctx);
          res->ib_ctx = NULL;
        }
      if (dev_list)
        {
          ibv_free_device_list (dev_list);
          dev_list = NULL;
        }

    }
  return rc;
}


/* -------------------- QP_init, QP_create ----------------- */

// initialize qp
static void
QP_init (struct QP *res) {
  memset (res, 0, sizeof *res);
}


// create qp based on dev
static int
QP_create(struct QP *res,struct dev_resource *dev)
{
  res->dev = dev;

  struct ibv_qp_init_attr qp_init_attr;

  res->pd = dev->pd;
  res->mr = dev->mr;

  int rc = 0;
  int cq_size = 1;
  res->cq = ibv_create_cq (dev->ib_ctx,cq_size,NULL,NULL,0);
  if(!res->cq) {
    fprintf (stderr, "failed to create CQ with %u entries\n", cq_size);
    rc = 1;
    goto resources_create_exit;
  }

  memset (&qp_init_attr, 0, sizeof (qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 1;
  qp_init_attr.send_cq = res->cq;
  qp_init_attr.recv_cq = res->cq;
  qp_init_attr.cap.max_send_wr = 1;
  qp_init_attr.cap.max_recv_wr = 1;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;
  res->qp = ibv_create_qp (res->pd, &qp_init_attr);
  if (!res->qp)
    {
      fprintf (stderr, "failed to create QP\n");
      rc = 1;
      goto resources_create_exit;
    }

 resources_create_exit:
  if(rc) {

    /* Error encountered, cleanup */
    if (res->qp)
      {
	ibv_destroy_qp (res->qp);
	res->qp = NULL;
      }
    if(res->cq) {
      ibv_destroy_cq(res->cq);
      res->cq  = NULL;
    }
  }

  return rc;
}

/* --------------------------- Modify qp status (init, rtr, rts) -------------------------- */

static int
modify_qp_to_init (struct ibv_qp *qp)
{
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset (&attr, 0, sizeof (attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = rdma_config.ib_port;
  attr.pkey_index = 0;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
    IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

  flags =
    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to INIT\n");
  return rc;
}


static int
modify_qp_to_rtr (struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid,
                  uint8_t * dgid)
{
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset (&attr, 0, sizeof (attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_256;
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 0x12;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = dlid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = rdma_config.ib_port;
  if (rdma_config.gid_idx >= 0)
    {
      attr.ah_attr.is_global = 1;
      attr.ah_attr.port_num = 1;
      memcpy (&attr.ah_attr.grh.dgid, dgid, 16);
      attr.ah_attr.grh.flow_label = 0;
      attr.ah_attr.grh.hop_limit = 1;
      attr.ah_attr.grh.sgid_index = rdma_config.gid_idx;
      attr.ah_attr.grh.traffic_class = 0;
    }
  flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to RTR\n");
  return rc;
}


static int
modify_qp_to_rts (struct ibv_qp *qp)
{
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset (&attr, 0, sizeof (attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 0x12;
  attr.retry_cnt = 6;
  attr.rnr_retry = 0;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  attr.max_dest_rd_atomic = 1;
  flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
    IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to RTS\n");
  return rc;
}

/* ------------------------------- connect qp ------------------------------ */
static int
connect_qp (struct QP *res,struct cm_con_data_t tmp_con_data)
{
  struct cm_con_data_t remote_con_data;
  int rc = 0;
  char temp_char;

  /* exchange using TCP sockets info required to connect QPs */

  remote_con_data.addr = ntohll (tmp_con_data.addr);
  remote_con_data.rkey = ntohl (tmp_con_data.rkey);
  remote_con_data.qp_num = ntohl (tmp_con_data.qp_num);
  remote_con_data.lid = ntohs (tmp_con_data.lid);
  memcpy (remote_con_data.gid, tmp_con_data.gid, 16);
  /* save the remote side attributes, we will need it for the post SR */
  res->remote_props = remote_con_data;

  if (rdma_config.gid_idx >= 0)
    {
      uint8_t *p = remote_con_data.gid;
      fprintf (stdout,
               "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
               p[10], p[11], p[12], p[13], p[14], p[15]);
    }
  /* modify the QP to init */
  rc = modify_qp_to_init (res->qp);
  if (rc)
    {
      fprintf (stderr, "change QP state to INIT failed\n");
      goto connect_qp_exit;
    }
  /* let the client post RR to be prepared for incoming messages */

  /* modify the QP to RTR */
  rc =
    modify_qp_to_rtr (res->qp, remote_con_data.qp_num, remote_con_data.lid,
                      remote_con_data.gid);
  if (rc)
    {
      fprintf (stderr, "failed to modify QP state to RTR\n");
      goto connect_qp_exit;
    }
  fprintf (stderr, "Modified QP state to RTR\n");
  rc = modify_qp_to_rts (res->qp);
  if (rc)
    {
      fprintf (stderr, "failed to modify QP state to RTR\n");
      goto connect_qp_exit;
    }
  fprintf (stdout, "QP state was change to RTS\n");
  /* sync to make sure that both sides are in states that they can connect to prevent packet loose */

 connect_qp_exit:
  return rc;
}

static int post_receive(struct QP *res, char* local_buf, size_t size) {
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr* bad_rr = nullptr;
    int rc;

    /* prepare the scatter/gather entry */
    memset (&sge, 0, sizeof (sge));
    sge.addr = (uintptr_t) local_buf;
    sge.length = size;
    sge.lkey = res->mr->lkey;
  /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = nullptr;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;

    /* post the receive request to the RQ*/
    rc = ibv_post_recv(res->qp, &rr, &bad_rr);
    if (rc)
        fprintf(stderr, "failed to post RR\n");
    else 
        fprintf(stdout, "Received Request was posted\n");
    return rc;
}

/* ----------------------------- post_send ------------------------------- */

static int
post_send (struct QP *res, int opcode,char* local_buf,size_t size,size_t remote_offset)
{
  struct ibv_send_wr sr;
  struct ibv_sge sge;
  struct ibv_send_wr *bad_wr = NULL;
  int rc;
  /* prepare the scatter/gather entry */
  memset (&sge, 0, sizeof (sge));
  sge.addr = (uintptr_t) local_buf;
  sge.length = size;
  sge.lkey = res->mr->lkey;
  /* prepare the send work request */
  memset (&sr, 0, sizeof (sr));
  sr.next = NULL;
  sr.wr_id = 0;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = static_cast<ibv_wr_opcode>(opcode);
  sr.send_flags = IBV_SEND_SIGNALED;
  // for IBV_WR_RDMA_WRITE_WITH_IMM
  if (opcode == IBV_WR_RDMA_WRITE_WITH_IMM) {
      sr.imm_data = 1;  // TODO: only for testing
  }
  if (opcode != IBV_WR_SEND)
    {
      sr.wr.rdma.remote_addr = res->remote_props.addr+remote_offset;
      sr.wr.rdma.rkey = res->remote_props.rkey;
    }
  /* there is a Receive Request in the responder side, so we won't get any into RNR flow */
  rc = ibv_post_send (res->qp, &sr, &bad_wr);
  if (rc)
    fprintf (stderr, "failed to post SR\n");
  else
    {
	switch (opcode)
	{
        case IBV_WR_SEND:
        fprintf (stdout, "Send Request was posted\n");
        break;
        case IBV_WR_RDMA_READ:
        fprintf (stdout, "RDMA Read Request was posted\n");
        break;
        case IBV_WR_RDMA_WRITE:
        fprintf (stdout, "RDMA Write Request was posted\n");
        break;
        default:
        fprintf (stdout, "Unknown Request was posted\n");
        break;
	}
    }
  return rc;
}

/* ----------------------------- poll_completion ------------------------------- */

static int
poll_completion (struct QP *res) {
  struct ibv_wc wc;
  unsigned long start_time_msec;
  unsigned long cur_time_msec;
  struct timeval cur_time;
  int poll_result;
  int rc = 0;
  /* poll the completion for a while before giving up of doing it .. */
  //  gettimeofday (&cur_time, NULL);
  //  start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);

  do
    {
      poll_result = ibv_poll_cq (res->cq, 1, &wc);
      //      gettimeofday (&cur_time, NULL);
      //      cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    }
  while ((poll_result == 0));
  //         && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
  if (poll_result < 0)
    {
      /* poll CQ failed */
      fprintf (stderr, "poll CQ failed\n");
      rc = 1;
    }
  else if (poll_result == 0)
    {
      /* the CQ is empty */
      fprintf (stderr, "completion wasn't found in the CQ after timeout\n");
      rc = 1;
    }
  else
    {
      /* CQE found */
      //      fprintf (stdout, "completion was found in CQ with status 0x%x\n",
      //               wc.status);
      /* check the completion status (here we don't care about the completion opcode */
      if (wc.status != IBV_WC_SUCCESS)
	{
	  fprintf (stderr,
		   "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
		   wc.status, wc.vendor_err);
	  rc = 1;
	}
    }
  return rc;
}

/* -------------------- Rdma operation --------------------- */
int RdmaResourcePair::rdmaOp(char* local,int size,uint64_t remote_offset,int op) {
    //simple wrapper function for handling rdma compare and swap

    if(post_send(res,op,local,size,remote_offset) ) {
      fprintf(stderr,"failed to post request.");
      assert(false);
    }
    if(::poll_completion(res)) {
      fprintf(stderr,"poll completion failed\n");
      assert(false);
    }
    //TODO! we need to
    return 0;
}

int RdmaResourcePair::poll_completion() {
    if(::poll_completion(res)) {
      fprintf(stderr,"poll completion failed\n");
      assert(false);
    }
}
int RdmaResourcePair::RdmaWrite(char* local,int size,uint64_t off) {
    return rdmaOp(local,size,off,IBV_WR_RDMA_WRITE);
}
int RdmaResourcePair::RdmaWriteWithImmediate(char* local,int size,uint64_t off) {
    return rdmaOp(local,size,off,IBV_WR_RDMA_WRITE_WITH_IMM);
}
int RdmaResourcePair::RdmaRead(char* local,int size,uint64_t off) {
    return rdmaOp(local,size,off,IBV_WR_RDMA_READ);
}

int RdmaResourcePair::post_receive(char* local, int size) {
    if (::post_receive(res, local, size)) {
      fprintf(stderr,"failed to post receive request.");
      assert(false);
    }
    return 0;
}

/* -------------------- Rdma Resource  --------------------- */
RdmaResourcePair::RdmaResourcePair(char* _mem, uint64_t _size) {
    buffer = _mem;
    size = _size;
    init();
}

void RdmaResourcePair::init() {
    dev = new dev_resource;
    
    // init and create device
    dev_resources_init(dev);
    if (dev_resources_create(dev, buffer, size)) {
      fprintf(stderr,"failed to create dev resources");
      assert(false);
    }

    // init and create qp 
    res = new QP;
    QP_init(res);
    if (QP_create(res, dev)) {
	    fprintf (stderr, "failed to create qp\n");
	    assert(false);
    }
}

void RdmaResourcePair::exchange_info(const std::string& remote_name, int port) {
    zmq::context_t ctx;

    zmq::socket_t pull(ctx, ZMQ_PULL);
    pull.bind("tcp://*:"+std::to_string(port));

    zmq::socket_t push(ctx, ZMQ_PUSH);
    push.connect("tcp://"+remote_name+":"+std::to_string(port));

    cm_con_data_t local_con_data = get_local_con_data(res);
    zmq::message_t msg(sizeof(local_con_data));
    memcpy(msg.data(), &local_con_data, sizeof(local_con_data));
    push.send(msg);
    print_conn(local_con_data);

    cm_con_data_t remote_con_data;
    pull.recv(&remote_con_data, sizeof(cm_con_data_t), 0);
    print_conn(remote_con_data);
    if (connect_qp(res, remote_con_data)) {
        fprintf (stderr, "failed to connect QPs\n");
        assert(false);
        exit(-1);
    }
    fprintf(stdout,"connection done------------\n");

    // TODO: delete it
    // barrier(remote_name);
}

void RdmaResourcePair::barrier(const std::string& remote_name) {
    zmq::context_t ctx;

    zmq::socket_t pull(ctx, ZMQ_PULL);
    pull.bind("tcp://*:12123");

    zmq::socket_t push(ctx, ZMQ_PUSH);
    push.connect("tcp://"+remote_name+":12123");

    zmq::message_t msg, recv_msg;  // dummy message
    push.send(msg);

    pull.recv(&recv_msg);
}
void RdmaResourcePair::print_conn(const cm_con_data_t& data) {
    std::cout << "addr: " << data.addr << " rkey: "  << data.rkey << " qp_num: " \
        << data.qp_num <<  " lid: " << data.lid << std::endl;
}
void RdmaResourcePair::busy_read(char** buf) {
    *buf = reinterpret_cast<char*>(get_buf());
    while(1){
        if(strlen(*buf) > 0){
            break;
        }
    }
    return;
}

void RdmaResourcePair::rdma_read(char** buf) {
    *buf = reinterpret_cast<char*>(get_buf());
    return;
}

char* RdmaResourcePair::get_buf() {
    return buffer;
}

