/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_OCL_REF_POOLING_HPP
#define GPU_OCL_REF_POOLING_HPP

#include "common/c_types_map.hpp"
#include "common/primitive.hpp"
#include "gpu/compute/compute.hpp"
#include "gpu/gpu_pooling_pd.hpp"
#include "gpu/ocl/ocl_resource.hpp"
#include "gpu/ocl/ocl_stream.hpp"
#include "gpu/ocl/ocl_utils.hpp"
#include "gpu/primitive_conf.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct ref_pooling_fwd_t : public primitive_t {
    struct pd_t : public gpu_pooling_fwd_pd_t {
        pd_t(const pooling_desc_t *adesc, const primitive_attr_t *attr,
                const pooling_fwd_pd_t *hint_fwd_pd)
            : gpu_pooling_fwd_pd_t(adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T("ocl:ref", ref_pooling_fwd_t);

        status_t init(engine_t *engine) {
            using namespace data_type;
            using namespace prop_kind;
            using namespace alg_kind;
            auto src_data_t = src_md()->data_type;
            auto dst_data_t = dst_md()->data_type;
            auto acc_data_t = desc()->accum_data_type;

            bool ok = set_default_params() == status::success
                    && utils::one_of(desc()->prop_kind, forward_training,
                            forward_inference)
                    && utils::one_of(desc()->alg_kind, pooling_max,
                            pooling_avg_include_padding,
                            pooling_avg_exclude_padding)
                    && (utils::everyone_is(
                                f32, src_data_t, dst_data_t, acc_data_t)
                            || utils::everyone_is(
                                    f16, src_data_t, dst_data_t, acc_data_t)
                            || utils::everyone_is(bf16, src_data_t, dst_data_t)
                            || utils::everyone_is(u8, src_data_t, dst_data_t)
                            || utils::everyone_is(s8, src_data_t, dst_data_t))
                    && IMPLICATION(utils::one_of(src_data_t, f16),
                            desc()->prop_kind == forward_inference)
                    && IMPLICATION(src_data_t == u8 || src_data_t == s8,
                            desc()->accum_data_type == s32)
                    && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            bool is_training = desc_.prop_kind == forward_training;
            if (desc()->alg_kind == pooling_max && is_training)
                init_default_ws(s32);

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        pool_conf_t conf;
        offsets_t off;
    };

    ref_pooling_fwd_t(const pd_t *apd) : primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        auto *compute_engine
                = utils::downcast<compute::compute_engine_t *>(engine);

        compute::kernel_ctx_t kernel_ctx;
        status_t status = pd()->init_kernel_ctx(kernel_ctx);
        CHECK(status);

        compute_engine->create_binary(&binary_, "ref_pooling_fwd", kernel_ctx);
        if (!binary_) return status::runtime_error;

        return status::success;
    }

    status_t create_resource(
            engine_t *engine, resource_mapper_t &mapper) const override {
        if (mapper.has_resource(this)) return status::success;
        auto r = utils::make_unique<ocl_resource_t>();
        if (!r) return status::out_of_memory;
        CHECK(r->create_kernel_and_add(engine, binary_));
        mapper.add(this, std::move(r));
        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_forward(ctx);
    }

private:
    status_t execute_forward(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::binary_t binary_;
};

struct ref_pooling_bwd_t : public primitive_t {
    struct pd_t : public gpu_pooling_bwd_pd_t {
        pd_t(const pooling_desc_t *adesc, const primitive_attr_t *attr,
                const pooling_fwd_pd_t *hint_fwd_pd)
            : gpu_pooling_bwd_pd_t(adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T("ocl:ref:any", ref_pooling_bwd_t);

        status_t init(engine_t *engine) {
            using namespace prop_kind;
            using namespace alg_kind;

            bool ok = set_default_params() == status::success
                    && utils::one_of(desc()->prop_kind, backward_data)
                    && utils::one_of(desc()->alg_kind, pooling_max,
                            pooling_avg_include_padding,
                            pooling_avg_exclude_padding)
                    && (utils::everyone_is(data_type::f32,
                                diff_dst_md()->data_type,
                                diff_src_md()->data_type)
                            || utils::everyone_is(data_type::bf16,
                                    diff_dst_md()->data_type,
                                    diff_src_md()->data_type))
                    && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            if (desc()->alg_kind == pooling_max) {
                init_default_ws(data_type::s32);
                if (!compare_ws(hint_fwd_pd_)) return status::unimplemented;
            }

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        pool_conf_t conf;
        offsets_t off;
    };

    ref_pooling_bwd_t(const pd_t *apd) : primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        auto *compute_engine
                = utils::downcast<compute::compute_engine_t *>(engine);

        compute::kernel_ctx_t kernel_ctx;
        status_t status = pd()->init_kernel_ctx(kernel_ctx);
        CHECK(status);

        compute_engine->create_binary(&binary_, "ref_pooling_bwd", kernel_ctx);
        if (!binary_) return status::runtime_error;

        return status::success;
    }

    status_t create_resource(
            engine_t *engine, resource_mapper_t &mapper) const override {
        if (mapper.has_resource(this)) return status::success;
        auto r = utils::make_unique<ocl_resource_t>();
        if (!r) return status::out_of_memory;
        CHECK(r->create_kernel_and_add(engine, binary_));
        mapper.add(this, std::move(r));
        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward(ctx);
    }

private:
    status_t execute_backward(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::binary_t binary_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
