# Use an official Python runtime as a parent image
FROM python:3.9-slim
 
# Set environment variables
ENV JUPYTERLAB_VERSION=3.4.3
 
# Replace 'jupyteruser' with your desired username
ENV USERNAME=jupyteruser
ENV USERHOME=/home/${USERNAME}
ENV WORKSPACE=${USERHOME}/workspace
 
# Use /tmp to avoid permission issues during pip install
WORKDIR /tmp
 
# Copy the requirements file into the container
COPY requirements.txt /tmp/requirements.txt
 
# Install JupyterLab and any dependencies in the requirements file
RUN pip install --no-cache-dir -r requirements.txt \
&& pip install --no-cache-dir jupyterlab==${JUPYTERLAB_VERSION}
 
# Create a non-root user and give them ownership of their home directory and workspace
RUN adduser --disabled-password --gecos '' ${USERNAME} \
&& mkdir -p ${WORKSPACE} \
&& chown -R ${USERNAME}:${USERNAME} ${USERHOME}
 
# Switch to the non-root user
USER ${USERNAME}
 
# Set the working directory to the workspace
WORKDIR ${WORKSPACE}
 
# Expose the port JupyterLab will use
EXPOSE 8888
 
# Start JupyterLab
# Use --ip=0.0.0.0 to listen on all interfaces inside the container
CMD ["jupyter", "lab", "--ip=0.0.0.0", "--no-browser", "--allow-root"]