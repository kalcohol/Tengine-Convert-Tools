/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * License); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * AS IS BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (c) 2017, Open AI Lab
 * Author: haitao@openailab.com
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <iostream>
#include <string>

#include "tengine_errno.hpp"
#include "tengine_config.hpp"
#include "tengine_plugin.hpp"

#include "graph_perf.hpp"
#include "static_graph.hpp"
#include "graph_executor.hpp"

#include "serializer.hpp"

#include "tengine_c_api.h"
#include "tengine_c_compat.h"
#include "tengine_c_helper.hpp"
#include "exec_context.hpp"
#include "dev_executor.hpp"
#include "dev_proposal.hpp"
#include "tensor_mem.hpp"
#include "custom_kernel.hpp"
#include "operator/generic.hpp"

using namespace TEngine;

#define TO_BE_IMPLEMENTED XLOG_WARN() << "TODO: " << __FUNCTION__ << " to be implemented\n"
#define ATTR_API_GRAPH "API_GRAPH"
#define ATTR_PRIV_CONTEXT "PRIV_CONTEXT"

int init_tengine(void)
{
    static int initialized = 0;
    static std::mutex init_mutex;

    if (initialized)
        return 0;

    init_mutex.lock();

    if (initialized)
    {
        init_mutex.unlock();
        return 0;
    }

    initialized = 1;

    // set the default online cpu according to env var
    const char* cpu_list_str = std::getenv("TENGINE_CPU_LIST");

    if (cpu_list_str)
    {
        LOG_INFO() << "ENV SET: [" << cpu_list_str << "]\n";
        set_cpu_list(cpu_list_str);
    }

    if (InitAllPlugin() < 0)
    {
        return -1;
    }

    if (TEnginePlugin::InitModule() < 0)
    {
        LOG_ERROR() << "init module failed\n";
        set_tengine_errno(ENOSYS);

        return -1;
    }

    TEngineConfig::Set("exec.engine", "generic", true);

    init_mutex.unlock();

#ifdef ENABLE_ONLINE_REPORT
    init_tengine_report_mgr();
    do_tengine_report(ACTION_INIT);
#endif
    return 0;
}

static int get_mem_line_int(char* line)
{
    char* p = strtok(line, " \t");

    p = strtok(NULL, " \t");

    return strtoul(p, NULL, 10);
}

static void get_sustain_mem(int* all_rss, int* ex_rss, int* anon_rss)
{
    int pid = getpid();
    int vm_size = 0;
    int rss_size = 0;
    int pss_size = 0;
    int ex_vm_size = 0;
    int ex_rss_size = 0;
    int ex_pss_size = 0;
    int nomap_vm_size = 0;
    int nomap_rss_size = 0;
    int nomap_pss_size = 0;

    char fname[128];

    sprintf(fname, "/proc/%d/smaps", pid);

    FILE* fp = fopen(fname, "r");

    char line[1024];

    while (fgets(line, 1024, fp))
    {
        /* 33f69000-33fbc000 rw-p 00000000 00:00 0 */
        int nomap_block = 0;
        if (strncmp(line, "Name:", strlen("Name:")) == 0)
            if (fgets(line, 1024, fp) == NULL)
                break;

        char* p = strtok(line, " \t");

        p = strtok(NULL, " \t");
        p = strtok(NULL, " \t");
        p = strtok(NULL, " \t");

        if (!strcmp(p, "00:00"))
            nomap_block = 1;

        if (fgets(line, 1024, fp) == NULL)
            break;

        int cur_vm_size = get_mem_line_int(line);

        /* search VmFlags: the last line in one block*/
        while (fgets(line, 1024, fp) && strncmp(line, "Rss:", strlen("Rss:")))
            ;

        int cur_rss_size = get_mem_line_int(line);

        /* Pss:                   4 kB */
        while (fgets(line, 1024, fp) && strncmp(line, "Pss:", strlen("Pss:")))
            ;

        int cur_pss_size = get_mem_line_int(line);

        /* search VmFlags: the last line in one block*/
        while (fgets(line, 1024, fp) && strncmp(line, "VmFlags:", strlen("VmFlags:")))
            ;

        if (strstr(line, "ex"))
        {
            ex_vm_size += cur_vm_size;
            ex_rss_size += cur_rss_size;
            ex_pss_size += cur_pss_size;
        }

        if (nomap_block)
        {
            nomap_vm_size += cur_vm_size;
            nomap_rss_size += cur_rss_size;
            nomap_pss_size += cur_pss_size;
        }

        vm_size += cur_vm_size;
        rss_size += cur_rss_size;
        pss_size += cur_pss_size;
    }

    fclose(fp);

    *all_rss = pss_size;
    *anon_rss = nomap_pss_size;
    *ex_rss = ex_pss_size;
}

static int get_peak_mem(void)
{
    int peak_mem = 0;
    int pid = getpid();

    char fname[128];

    sprintf(fname, "/proc/%d/status", pid);

    FILE* fp = fopen(fname, "r");

    char line[128];

    while (fgets(line, 128, fp))
    {
        if (strncmp(line, "VmHWM:", strlen("VmHWM:")))
            continue;

        char* p = line + strlen("VmHWM:");

        peak_mem = strtoul(p, NULL, 10);
    }

    fclose(fp);

    return peak_mem;
}

void release_tengine(void)
{
    TEnginePlugin::ReleaseModule();
#ifdef ENABLE_ONLINE_REPORT
    do_tengine_report(ACTION_RELEASE);
    release_tengine_report_mgr();
#endif
}

/*
   model_format == NULL, only happens when create a new graph, instead of loading a model
   model_format == "xxx:m", it is to load model from memory instead of file
*/

graph_t create_graph(context_t context, const char* model_format, const char* fname, ...)
{
    va_list argp;
    va_start(argp, fname);

    bool new_context_created = false;

    ExecContext* exec_context = reinterpret_cast<ExecContext*>(context);

    if (exec_context == nullptr)
    {
        if (fname)
            exec_context = ( ExecContext* )create_context(fname, 0);
        else
            exec_context = ( ExecContext* )create_context("FOR_EMPTY", 0);

        if (exec_context == nullptr)
        {
            set_tengine_errno(ENODEV);
            return nullptr;
        }

        new_context_created = true;
    }

    graph_t graph = nullptr;
    const char* graph_name = nullptr;
    char tmp_buf[128];

    if (model_format == nullptr)
    {
        sprintf(tmp_buf, "graph0x%lx:%u", ( long )context, rand());
        graph_name = tmp_buf;

        graph = create_graph_in_context(exec_context, graph_name, nullptr);
    }
    else
    {
        bool file_model = true;
        std::string real_format(model_format);

        auto pos = real_format.rfind(':');

        if (pos != std::string::npos && (pos == real_format.size() - 2) && real_format[pos + 1] == 'm')
        {
            file_model = false;
            graph_name = "mem";

            real_format.resize(pos);
        }
        else
        {
            graph_name = fname;
        }

        // sprintf(tmp_buf, "%p:", ( void* )exec_context);

        std::string model_name(tmp_buf);

        model_name += graph_name;

        bool model_loaded = false;

        if (file_model)
        {
            if (vload_file_model(exec_context, model_name.c_str(), model_format, fname, argp) == 0)
                model_loaded = true;
        }
        else
        {
            const void* addr = ( const void* )fname;
            int mem_size = va_arg(argp, int);

            if (vload_mem_model(exec_context, model_name.c_str(), real_format.c_str(), addr, mem_size, argp) == 0)
                model_loaded = true;
        }

        if (model_loaded)
            graph = create_graph_in_context(exec_context, graph_name, model_name.c_str());
    }

    if (graph == nullptr)
    {
        if (new_context_created)
            delete exec_context;
        return nullptr;
    }

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);
    Graph* real_graph = executor->GetGraph();

    if (new_context_created)
        real_graph->SetAttr(ATTR_PRIV_CONTEXT, exec_context);

    return graph;
}

int save_graph(graph_t graph, const char* model_format, const char* fname, ...)
{
    va_list argp;
    va_start(argp, fname);

    return save_graph_internal(graph, model_format, fname, argp);
}

int set_graph_layout(graph_t graph, int layout_type)
{
    if (layout_type != TENGINE_LAYOUT_NCHW && layout_type != TENGINE_LAYOUT_NHWC)
    {
        LOG_ERROR() << "unknown layout type: " << layout_type << "\n";
        set_tengine_errno(EINVAL);
        return -1;
    }

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EACCES);
        return -1;
    }

    Graph* real_graph = executor->GetGraph();

    real_graph->SetLayout(layout_type);

    return 0;
}

int set_graph_input_node(graph_t graph, const char* input_nodes[], int input_number)
{
    if (input_number <= 0)
    {
        set_tengine_errno(EINVAL);
        return -1;
    }

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EACCES);
        return -1;
    }

    Graph* real_graph = executor->GetGraph();

    /* check if all input nodes are defined in graph */
    for (int i = 0; i < input_number; i++)
    {
        if (!real_graph->FindNode(input_nodes[i]))
        {
            set_tengine_errno(ENOENT);
            return -1;
        }
    }

    /* set the new input nodes */
    real_graph->ResetInputNode();

    for (int i = 0; i < input_number; i++)
        real_graph->AddInputNode(input_nodes[i]);

    return 0;
}

int set_graph_output_node(graph_t graph, const char* output_nodes[], int output_number)
{
    if (output_number <= 0)
    {
        set_tengine_errno(EINVAL);
        return -1;
    }

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EACCES);
        return -1;
    }

    Graph* real_graph = executor->GetGraph();

    /* check if all output nodes are defined in graph */
    for (int i = 0; i < output_number; i++)
    {
        if (!real_graph->FindNode(output_nodes[i]))
        {
            set_tengine_errno(ENOENT);
            return -1;
        }
    }

    /* set the new output nodes */
    real_graph->ResetOutputNode();

    for (int i = 0; i < output_number; i++)
        real_graph->AddOutputNode(output_nodes[i]);

    return 0;
}

graph_t merge_graph(int graph_num, graph_t graph0, graph_t graph1, ...)
{
    std::vector<GraphExecutor*> exec_list;

    exec_list.push_back(reinterpret_cast<GraphExecutor*>(graph0));
    exec_list.push_back(reinterpret_cast<GraphExecutor*>(graph1));

    va_list argp;
    va_start(argp, graph1);

    for (int i = 2; i < graph_num; i++)
    {
        graph_t gph = va_arg(argp, graph_t);
        GraphExecutor* sec_executor = reinterpret_cast<GraphExecutor*>(gph);

        exec_list.push_back(sec_executor);
    }

    /* check if prerun has been done */

    GraphExecutor* executor = exec_list[0];
    ExecContext* exec_context = ( ExecContext* )executor->GetExecAttr()->exec_context;

    for (int i = 1; i < graph_num; i++)
    {
        GraphExecutor* sec_executor = exec_list[1];

        if (sec_executor->PrerunDone())
        {
            set_tengine_errno(EPERM);
            return nullptr;
        }

        ExecContext* sec_context = ( ExecContext* )sec_executor->GetExecAttr()->exec_context;

        if (sec_context != exec_context)
        {
            set_tengine_errno(EINVAL);
            return nullptr;
        }
    }

    /* merge graph */
    return do_merge_graph(exec_list);
}

int destroy_graph(graph_t graph)
{
    const char* model_name = get_model_name(graph);

    if (model_name)
        remove_model(model_name);

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);
    Graph* real_graph = executor->GetGraph();

    ExecContext* exec_context = nullptr;

    if (real_graph->ExistAttr(ATTR_PRIV_CONTEXT))
    {
        exec_context = any_cast<ExecContext*>(real_graph->GetAttr(ATTR_PRIV_CONTEXT));
    }

    destroy_runtime_graph(graph);

    delete exec_context;

    return 0;
}

int get_graph_input_node_number(graph_t graph)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    Graph* real_graph = executor->GetOptimizedGraph();

    return real_graph->input_nodes.size();
}

node_t get_graph_input_node(graph_t graph, int idx)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    Graph* real_graph = executor->GetOptimizedGraph();

    if (idx >= ( int )real_graph->input_nodes.size())
    {
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    Node* node = real_graph->input_nodes[idx];

    node->SetAttr(ATTR_API_GRAPH, executor);

    return node;
}

int get_graph_output_node_number(graph_t graph)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    Graph* real_graph = executor->GetOptimizedGraph();

    return real_graph->output_nodes.size();
}

node_t get_graph_output_node(graph_t graph, int idx)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    Graph* real_graph = executor->GetOptimizedGraph();

    if (idx >= ( int )real_graph->output_nodes.size())
    {
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    Node* node = real_graph->output_nodes[idx];

    node->SetAttr(ATTR_API_GRAPH, executor);

    return node;
}

tensor_t get_graph_output_tensor(graph_t graph, int output_node_idx, int tensor_idx)
{
    node_t node = get_graph_output_node(graph, output_node_idx);

    if (node == nullptr)
        return nullptr;

    return get_node_output_tensor(node, tensor_idx);
}

tensor_t get_graph_input_tensor(graph_t graph, int input_node_idx, int tensor_idx)
{
    node_t node = get_graph_input_node(graph, input_node_idx);

    if (node == nullptr)
        return nullptr;

    return get_node_output_tensor(node, tensor_idx);
}

node_t create_graph_node(graph_t graph, const char* node_name, const char* op_name)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EACCES);
        return nullptr;
    }

    Graph* real_graph = executor->GetGraph();

    /* check if duplicate name */
    if (real_graph->FindNode(node_name))
    {
        set_tengine_errno(EEXIST);
        return nullptr;
    }

    Operator* op = OpManager::CreateOp(op_name);

    if (op == nullptr)
    {
        set_tengine_errno(ENOENT);
        return nullptr;
    }

    Node* new_node = new Node(node_name);

    if (new_node == nullptr)
    {
        set_tengine_errno(ENOMEM);
        return new_node;
    }

    new_node->SetOp(op);

    real_graph->AddNode(new_node);

    new_node->SetAttr(ATTR_API_GRAPH, executor);

    return new_node;
}

node_t get_graph_node(graph_t graph, const char* node_name)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    Node* node = executor->FindNode(node_name);

    node->SetAttr(ATTR_API_GRAPH, executor);

    return node;
}

const char* get_node_name(node_t node)
{
    Node* real_node = reinterpret_cast<Node*>(node);

    return real_node->GetName().c_str();
}

const char* get_node_op(node_t node)
{
    Node* real_node = reinterpret_cast<Node*>(node);

    return real_node->GetOp()->GetName().c_str();
}

void release_graph_node(node_t node)
{
    Node* real_node = reinterpret_cast<Node*>(node);

    if (!real_node->ExistAttr(ATTR_API_GRAPH))
    {
        /* this may happen when one node is gotten from two interfaces */
        LOG_INFO() << "node: " << real_node->GetName() << " do not have API Graph attribute\n";
        return;
    }

    real_node->RemoveAttr(ATTR_API_GRAPH);
}

tensor_t get_node_input_tensor(node_t node, int input_idx)
{
    Node* real_node = reinterpret_cast<Node*>(node);
    int input_num = real_node->GetInputNum();

    if (input_idx >= input_num)
    {
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    return real_node->GetInputTensor(input_idx);
}

tensor_t get_node_output_tensor(node_t node, int output_idx)
{
    Node* real_node = reinterpret_cast<Node*>(node);
    int output_num = real_node->GetOutputNum();

    if (output_idx >= output_num)
    {
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    return real_node->GetOutputTensor(output_idx);
}

int set_node_input_tensor(node_t node, int input_idx, tensor_t tensor)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);
    Node* real_node = reinterpret_cast<Node*>(node);

    real_node->SetInputPort(input_idx, real_tensor);

    NodePort* port = real_node->GetInputPort(input_idx);

    real_tensor->consumer.push_back(port);

    return 0;
}

/* please set the tensor type when attach a tensor to a node */
int set_node_output_tensor(node_t node, int output_idx, tensor_t tensor, int tensor_type)
{
    Node* real_node = reinterpret_cast<Node*>(node);
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    real_node->SetOutputPort(output_idx, real_tensor);

    real_tensor->SetType(tensor_type);

    NodePort* port = real_node->GetOutputPort(output_idx);

    real_tensor->producer = port;

    return 0;
}

int get_node_output_number(node_t node)
{
    Node* real_node = reinterpret_cast<Node*>(node);

    return real_node->GetOutputNum();
}

int get_node_input_number(node_t node)
{
    Node* real_node = reinterpret_cast<Node*>(node);

    return real_node->GetInputNum();
}

int get_graph_node_number(graph_t graph)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);
    Graph* real_graph = executor->GetOptimizedGraph();

    return real_graph->seq_nodes.size();
}

node_t get_graph_node_by_idx(graph_t graph, int node_idx)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);
    Graph* real_graph = executor->GetOptimizedGraph();

    int node_num = real_graph->seq_nodes.size();

    if (node_idx < 0 || node_idx >= node_num)
    {
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    Node* node = real_graph->seq_nodes[node_idx];

    node->SetAttr(ATTR_API_GRAPH, executor);

    return node;
}

int add_node_attr(node_t node, const char* attr_name, const char* type_name, int size)
{
    /* first check if the attribute exists*/
    void* buf = malloc(size);

    int ret = get_node_attr_generic(node, attr_name, type_name, buf, size);

    free(buf);

    if (ret == 0)
    {
        set_tengine_errno(EEXIST);
        return -1;
    }

    return node_add_attr(node, attr_name, type_name, size);
}

int get_node_attr_int(node_t node, const char* attr_name, int* attr_val)
{
    return get_node_attr_generic(node, attr_name, typeid(int).name(), attr_val, sizeof(int));
}

int get_node_attr_float(node_t node, const char* attr_name, float* attr_val)
{
    return get_node_attr_generic(node, attr_name, typeid(float).name(), attr_val, sizeof(float));
}

int get_node_attr_pointer(node_t node, const char* attr_name, void* attr_val)
{
    return get_node_attr_generic(node, attr_name, nullptr, attr_val, sizeof(void*));
}

int get_node_attr_generic(node_t node, const char* attr_name, const char* type_name, void* buf, int size)
{
    return node_get_attr_generic(node, attr_name, type_name, buf, size);
}

int set_node_attr_int(node_t node, const char* attr_name, const int* attr_val)
{
    return set_node_attr_generic(node, attr_name, typeid(int).name(), attr_val, sizeof(int));
}

int set_node_attr_float(node_t node, const char* attr_name, const float* attr_val)
{
    return set_node_attr_generic(node, attr_name, typeid(float).name(), attr_val, sizeof(float));
}

int set_node_attr_pointer(node_t node, const char* attr_name, const void* attr_val)
{
    return set_node_attr_generic(node, attr_name, nullptr, attr_val, sizeof(void*));
}

int set_node_attr_generic(node_t node, const char* attr_name, const char* type_name, const void* buf, int size)
{
    return node_set_attr_generic(node, attr_name, type_name, buf, size);
}

tensor_t create_graph_tensor(graph_t graph, const char* tensor_name, int data_type)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EACCES);
        return nullptr;
    }

    if (data_type < TENGINE_DT_FP32 || data_type > TENGINE_DT_INT16)
    {
        LOG_ERROR() << "unknown data type: " << data_type << "\n";
        set_tengine_errno(EINVAL);
        return nullptr;
    }

    Graph* real_graph = executor->GetGraph();

    if (real_graph->FindTensor(tensor_name))
    {
        set_tengine_errno(EEXIST);
        return nullptr;
    }

    Tensor* new_tensor = new Tensor(tensor_name);

    if (new_tensor == nullptr)
    {
        set_tengine_errno(ENOMEM);
        return nullptr;
    }

    new_tensor->SetDataType(data_type);
    new_tensor->SetType(TENSOR_TYPE_CONST);
    new_tensor->GetShape().SetDataLayout(real_graph->GetLayout());

    real_graph->AddTensor(new_tensor);

    return new_tensor;
}

tensor_t get_graph_tensor(graph_t graph, const char* tensor_name)
{
    GraphExecutor* executor = static_cast<GraphExecutor*>(graph);
    Tensor* tensor = executor->FindTensor(tensor_name);

    if (tensor == nullptr)
        set_tengine_errno(ENOENT);

    return tensor;
}

int set_tensor_shape(tensor_t tensor, const int dims[], int dim_number)
{
    std::vector<int> dim;

    for (int i = 0; i < dim_number; i++)
        dim.push_back(dims[i]);

    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    TShape shape = real_tensor->GetShape();

    shape.SetDim(dim);

    real_tensor->Reshape(shape);

    return 0;
}

int get_tensor_shape(tensor_t tensor, int dims[], int dim_number)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    TShape& shape = real_tensor->GetShape();

    std::vector<int>& dim = shape.GetDim();

    int dim_size = dim.size();

    if (dim_size > dim_number)
    {
        set_tengine_errno(EINVAL);
        return -1;
    }

    for (int i = 0; i < dim_size; i++)
        dims[i] = dim[i];

    return dim_size;
}

int get_tensor_buffer_size(tensor_t tensor)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    return real_tensor->GetTotalSize();
}

void* get_tensor_buffer(tensor_t tensor)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    return get_tensor_mem(real_tensor);
}

int set_tensor_buffer(tensor_t tensor, void* buffer, int buffer_size)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    set_tensor_mem(real_tensor, buffer, buffer_size, nullptr);

    return 0;
}

int get_tensor_data(tensor_t tensor, void* output_data, int data_size)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);
    int buf_size = get_tensor_buffer_size(real_tensor);
    void* buf = get_tensor_buffer(tensor);

    if (buf_size > data_size)
    {
        set_tengine_errno(ENOMEM);
        return -1;
    }

    if (buf == nullptr)
    {
        set_tengine_errno(ENODATA);
        return -1;
    }

    memcpy(output_data, buf, buf_size);

    return 0;
}

int set_tensor_data(tensor_t tensor, const void* input_data, int data_size)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);
    int buf_size = get_tensor_buffer_size(real_tensor);
    void* buf = get_tensor_buffer(tensor);

    if (buf_size < data_size || buf == nullptr)
    {
        set_tengine_errno(ENOMEM);
        return -1;
    }

    memcpy(buf, input_data, data_size);

    return 0;
}

int get_tensor_data_type(tensor_t tensor)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    return real_tensor->GetDataType();
}

int set_tensor_data_type(tensor_t tensor, int data_type)
{
    Tensor* real_tensor = reinterpret_cast<Tensor*>(tensor);

    if (data_type < TENGINE_DT_FP32 || data_type > TENGINE_DT_INT16)
    {
        set_tengine_errno(EINVAL);
        return -1;
    }

    real_tensor->SetDataType(data_type);

    return 0;
}

int set_graph_attr(graph_t graph, const char* attr_name, const void* buf, int size)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    return executor->SetGraphAttr(attr_name, buf, size);
}

int get_graph_attr(graph_t graph, const char* attr_name, void* buf, int size)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    return executor->GetGraphAttr(attr_name, buf, size);
}

int prerun_graph(graph_t graph)
{
    GraphExecutor* executor = static_cast<GraphExecutor*>(graph);

    if (executor->PrerunDone())
    {
        set_tengine_errno(EPERM);
        return -1;
    }

    if (executor->Prerun())
        return 0;

    return -1;
}

int wait_graph(graph_t graph, int try_wait)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    return executor->WaitGraph(try_wait);
}

int get_graph_exec_status(graph_t graph)
{
    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    return executor->GetExecStatus();
}

int set_graph_event_hook(graph_t graph, int event, event_handler_t cb_func, void* cb_arg)
{
    if (event < GRAPH_EXEC_START || event > GRAPH_EXEC_DONE)
    {
        set_tengine_errno(EINVAL);
        return -1;
    }

    GraphExecutor* executor = reinterpret_cast<GraphExecutor*>(graph);

    if (executor->SetEventHook(event, cb_func, cb_arg))
        return 0;
    else
        return -1;
}

const char* get_default_device(void)
{
    DevExecutor* dev;

    if (DevExecutorManager::GetDefaultDevExecutor(dev))
        return dev->GetName().c_str();

    set_tengine_errno(ENOENT);
    return nullptr;
}

/* real device interfaces */

context_t create_context(const char* context_name, int empty_context)
{
    ExecContext* exec_context = new ExecContext(context_name, empty_context);

    return exec_context;
}

void destroy_context(context_t context)
{
    ExecContext* exec_context = reinterpret_cast<ExecContext*>(context);

    delete exec_context;
}

void dump_graph(graph_t graph)
{
    GraphExecutor* executor = static_cast<GraphExecutor*>(graph);

    /* first: try to dump optimized graph */
    Graph* g = executor->GetOptimizedGraph();

    g->DumpGraph();
}
