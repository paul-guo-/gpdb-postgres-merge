#!/usr/bin/env python
# ----------------------------------------------------------------------
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
# ----------------------------------------------------------------------

"""Generate pipeline (default: gpdb_master-generated.yml) from template (default:
templates/gpdb-tpl.yml).

Python module requirements:
  - jinja2 (install through pip or easy_install)
"""

import os
import datetime
import argparse
import subprocess

from jinja2 import Environment, FileSystemLoader

PIPELINES_DIR = os.path.dirname(os.path.abspath(__file__))

TEMPLATE_ENVIRONMENT = Environment(
    autoescape=False,
    loader=FileSystemLoader(os.path.join(PIPELINES_DIR, 'templates')),
    trim_blocks=True,
    lstrip_blocks=True,
    variable_start_string='[[', # 'default {{ has conflict with pipeline syntax'
    variable_end_string=']]',
    extensions=['jinja2.ext.loopcontrols'])

def render_template(template_filename, context):
    """Render template"""
    return TEMPLATE_ENVIRONMENT.get_template(template_filename).render(context)

def create_pipeline():
    """Generate OS specific pipeline sections
    """
    if ARGS.test_trigger_false:
        test_trigger = "true"
    else:
        test_trigger = "false"

    stagger_sections = False
    if ARGS.pipeline_type == "prod" or len(ARGS.test_sections) > 2:
        stagger_sections = True

    context = {
        'template_filename': ARGS.template_filename,
        'generator_filename': os.path.basename(__file__),
        'timestamp': datetime.datetime.now(),
        'os_types': ARGS.os_types,
        'test_sections': ARGS.test_sections,
        'pipeline_type': ARGS.pipeline_type,
        'stagger_sections': stagger_sections,
        'test_trigger': test_trigger
    }

    with open(ARGS.output_filepath, 'w') as output:
        yml = render_template('pipeline_header.yml', context)
        output.write(yml)

    with open(ARGS.output_filepath, 'a') as output:
        yml = render_template(ARGS.template_filename, context)
        output.write(yml)


if __name__ == "__main__":
    PARSER = argparse.ArgumentParser(
        description='Generate Concourse Pipeline utility',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)

    PARSER.add_argument('-T', '--template',
                        action='store',
                        dest='template_filename',
                        default="gpdb-tpl.yml",
                        help='Name of template to use, in templates/')

    default_output_filename = "gpdb_master-generated.yml"
    PARSER.add_argument('-o', '--output',
                        action='store',
                        dest='output_filepath',
                        default=os.path.join(PIPELINES_DIR, default_output_filename),
                        help='Output filepath')

    PARSER.add_argument('-O', '--os_types',
                        action='store',
                        dest='os_types',
                        default=['centos6'],
                        choices=['centos6', 'centos7', 'sles', 'aix7', 'win', 'ubuntu16'],
                        nargs='+',
                        help='List of OS values to support')

    PARSER.add_argument('-t', '--pipeline_type',
                        action='store',
                        dest='pipeline_type',
                        default='dev',
                        help='Pipeline type (production="prod")')

    PARSER.add_argument('-a', '--test_sections',
                        action='store',
                        dest='test_sections',
                        choices=['ICW', 'CS', 'MPP', 'MM', 'DPM', 'UD'],
                        default=['ICW'],
                        nargs='+',
                        help='Select tests sections to run')

    PARSER.add_argument('-n', '--test_trigger_false',
                        action='store_false',
                        default=True,
                        help='Set test triggers to "false". This only applies to dev pipelines.')

    PARSER.add_argument('-u', '--user',
                        action='store',
                        dest='user',
                        default=os.getlogin(),
                        help='Developer userid to use for pipeline file name.')

    ARGS = PARSER.parse_args()

    if ARGS.pipeline_type == 'prod':
        ARGS.os_types = ['centos6', 'centos7', 'sles', 'aix7', 'win', 'ubuntu16']
        ARGS.test_sections = ['ICW', 'CS', 'MPP', 'MM', 'DPM', 'UD']

    # if generating a dev pipeline but didn't specify an output, don't overwrite the master pipeline
    if ARGS.pipeline_type != 'prod' and os.path.basename(ARGS.output_filepath) == default_output_filename:
        default_dev_output_filename = 'gpdb-' + ARGS.pipeline_type + '-' + ARGS.user + '.yml'
        ARGS.output_filepath = os.path.join(PIPELINES_DIR, default_dev_output_filename)

    MSG = '\n'
    MSG += '======================================================================\n'
    MSG += '  Generate Pipeline type: .. : %s\n' % ARGS.pipeline_type
    MSG += '  Pipeline file ............ : %s\n' % ARGS.output_filepath
    MSG += '  Template file ............ : %s\n' % ARGS.template_filename
    MSG += '  OS Types ................. : %s\n' % ARGS.os_types
    MSG += '  Test sections ............ : %s\n' % ARGS.test_sections
    MSG += '  test_trigger ............. : %s\n' % ARGS.test_trigger_false
    MSG += '======================================================================\n\n'
    if ARGS.pipeline_type == 'prod':
        MSG += 'NOTE: You can set the production pipelines with the following:\n\n'
        MSG += 'fly -t gpdb-prod \\\n'
        MSG += '    set-pipeline \\\n'
        MSG += '    -p gpdb_master \\\n'
        MSG += '    -c %s \\\n' % ARGS.output_filepath
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_common-ci-secrets.yml \\\n'
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_master-ci-secrets.yml\n\n'
        MSG += 'fly -t gpdb-prod \\\n'
        MSG += '    set-pipeline \\\n'
        MSG += '    -p gpdb_master_without_asserts \\\n'
        MSG += '    -c %s \\\n' % ARGS.output_filepath
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_common-ci-secrets.yml \\\n'
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_master_without_asserts-ci-secrets.yml\n' # pylint: disable=line-too-long
    else:
        MSG += 'NOTE: You can set the developer pipeline with the following:\n\n'
        MSG += 'fly -t gpdb-dev \\\n'
        MSG += '    set-pipeline \\\n'
        MSG += '    -p %s \\\n' % os.path.basename(ARGS.output_filepath).rsplit('.', 1)[0]
        MSG += '    -c %s \\\n' % ARGS.output_filepath
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_common-ci-secrets.yml \\\n'
        MSG += '    -l ~/workspace/continuous-integration/secrets/gpdb_master-ci-secrets.yml \\\n'
        MSG += '    -v tf-bucket-path=dev/' + ARGS.pipeline_type + '/ \\\n'
        MSG += '    -v bucket-name=gpdb5-concourse-builds-dev \\\n'
        MSG += '    -v gpdb-git-remote=<https://github.com/<github-user>/gpdb> \\\n'
        MSG += '    -v gpdb-git-branch=<branch-name>\n'

    create_pipeline()

    if ARGS.pipeline_type == 'prod':
        print "======================================================================"
        print "Validate Pipeline Release Jobs"
        print "----------------------------------------------------------------------"
        try:
            env = os.environ.copy()
            env['PIPELINE_FILE'] = ARGS.output_filepath
            validator_script = os.path.join(PIPELINES_DIR, "../scripts/validate_pipeline_release_jobs.py")
            subprocess.check_call(["python", validator_script], env=env)
        except subprocess.CalledProcessError:
            exit(1)

    print MSG

